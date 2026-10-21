/**
 * @file ResumeRunEnd.cpp
 * @brief Run-end report of the warm resume and the state snapshot.
 *
 * The resume and the snapshot writer live in network/state/WarmResume.cpp (networkTryWarmResume)
 * and network/state/StateSnapshotWrite.cpp (networkWriteStateSnapshot); the snapshot file format
 * is products/NetworkStateSnapshot.cpp.
 *
 * The table that orders the reports is network/runend/RunEnd.cpp.
 */

#include "network/runend/ResumeRunEnd.hpp"

#include "network/state/RunContext.hpp"
#include "network/driver/SetupResult.hpp"
#include "config/GnssConfig.hpp"
#include "network/carry/InvariantCarryDriver.hpp"
#include "network/runend/RunEnd.hpp"
#include "network/driver/RunGroups.hpp"
#include "network/driver/RunTypes.hpp"
#include "model/PppModel.hpp"

#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

namespace GnssProcessor {

// ---- Run-end reports ----

/**
 * @brief Run-end section: warm-resume restores: frame, coupling regressors, dense border.
 *
 * The warm resume's restores: the frame restore and slip warm-up, the coupling-regressor
 * restore with its first-fold census, and the dense-border restore.
 *
 * Gate: each line under its own condition:
 * `config_.networkResumeFrameRestore() || config_.networkResumeSlipWarmupEp() > 0`;
 * `config_.networkResumeCouplingRestore()` (the coupling line and the dense-border line);
 * `config_.networkResumeBorderRestore()`, or else dense-carry keys restored with no
 * restore armed.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkResumeRestoreRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const auto& C = v.ctx.counters;

    // Resume frame restore and slip warm-up: the identities restored at the re-coupling
    // edge.  0 anchors under an armed knob means the snapshot carried no frame rows (a
    // v1 file) or no resume happened.
    if (config_.networkResumeFrameRestore() || config_.networkResumeSlipWarmupEp() > 0)
        log->info("Network (resume frame restore): {} phase-anchor incumbent(s), {} sticky "
                  "pin target(s), {} clock-anchor sat state(s) ({} anchored), {} CLK-R "
                  "held-frame key(s), {} ledger entr(ies) primed, restored at the seam; MW/LLI drain diagnostic-only "
                  "for {} post-resume epoch(s) (knob {}).{}",
                  C.n_resume_frame_anchors, C.n_resume_frame_targets,
                  C.n_resume_frame_cta, C.n_resume_frame_cta_anchored,
                  C.n_resume_frame_clk_keys, C.n_resume_frame_ledger, C.n_resume_slip_warmup_epochs,
                  config_.networkResumeSlipWarmupEp(),
                  (config_.networkResumeFrameRestore() && C.n_resume_frame_anchors == 0)
                      ? "  ⚠ 0 anchors under an ARMED frame restore — the snapshot "
                        "carried no frame rows (pre-v2 writer?) or no warm resume "
                        "happened; check the resume log for the FRAME line."
                      : "");
    // Coupling-regressor restore, the re-coupling edge of the seam: what came back, and
    // what it bound to at the first fold.
    // Restoring (value, var) alone injects a carried leveling row as a plain diagonal
    // prior at the pure conditional variance, a combination no continuous epoch
    // produces, so the row drives the shared border by the full α‖M_d‖ at
    // α ≈ 1/cond_var ≈ 1e6 m⁻², about 90 times the clock temporal anchor's prior of
    // 1/9e-5 ≈ 1.1e4 m⁻².  This is the mechanism behind the first-fold clock-anchor trips,
    // so the readout is not how many rows came back but how many of their terms resolved
    // to a live column; both edges are printed here.
    //
    // The census vectors hold one entry per row of the first post-resume harvest (the
    // measure-only observatory networkResumeCouplingCensusRow), so they are reduced here
    // rather than accumulated online: nth_element is O(n), the run is over, and the
    // observatory stays byte-identical on every solve artifact.  The reduction works on
    // copies so the readout never permutes carried state.
    double rel_p50 = 0.0, rel_p90 = 0.0, rel_max = 0.0, rhs_p50 = 0.0, rhs_max = 0.0;
    {
        using Diff = std::vector<double>::difference_type;
        std::vector<double> cen_rel = ctx.carry.resume_cpl_census_rel;
        if (!cen_rel.empty()) {
            const std::size_t i50 = cen_rel.size() / 2;
            const std::size_t i90 = (cen_rel.size() * 9) / 10;   // < size for size >= 1
            std::nth_element(cen_rel.begin(),
                             cen_rel.begin() + static_cast<Diff>(i50),
                             cen_rel.end());
            rel_p50 = cen_rel[i50];
            std::nth_element(cen_rel.begin(),
                             cen_rel.begin() + static_cast<Diff>(i90),
                             cen_rel.end());
            rel_p90 = cen_rel[i90];
            rel_max = *std::max_element(cen_rel.begin(), cen_rel.end());
        }
        std::vector<double> cen_rhs = ctx.carry.resume_cpl_census_rhs;
        if (!cen_rhs.empty()) {
            const std::size_t j50 = cen_rhs.size() / 2;
            std::nth_element(cen_rhs.begin(),
                             cen_rhs.begin() + static_cast<Diff>(j50),
                             cen_rhs.end());
            rhs_p50 = cen_rhs[j50];
            rhs_max = *std::max_element(cen_rhs.begin(), cen_rhs.end());
        }
    }
    if (config_.networkResumeCouplingRestore())
        log->info("Network (resume coupling restore): {} L row(s) + {} Z row(s), {} "
                  "regressor(s) over a {}-column dictionary ({} orphan row(s)); {} "
                  "clock reinit seed(s) planted at the seam.  First fold: {} present, "
                  "{} departed/diverted ({:.1f} % present), {} border key(s) folded "
                  "DIAGONALLY (the known one-epoch C1 asymmetry).  First-harvest "
                  "census over {} row(s) / {} matched term(s): |M-M_d|/max|M_d| p50 "
                  "{:.3e}, p90 {:.3e}, max {:.3e}; |delta sum M*s_prev| p50 {:.3e} m, "
                  "max {:.3e} m ({} restored identit(ies) not re-harvested).{}",
                  C.n_resume_cpl_rows, C.n_resume_cpl_zrows, C.n_resume_cpl_terms,
                  C.n_resume_cpl_dict, C.n_resume_cpl_orphan,
                  C.n_resume_cpl_seed_reinit,
                  C.n_resume_cpl_seam_present, C.n_resume_cpl_seam_departed,
                  (C.n_resume_cpl_seam_present + C.n_resume_cpl_seam_departed) > 0
                      ? 100.0 * static_cast<double>(C.n_resume_cpl_seam_present)
                        / static_cast<double>(C.n_resume_cpl_seam_present
                                            + C.n_resume_cpl_seam_departed)
                      : 0.0,
                  C.n_resume_cpl_diag_border,
                  C.n_resume_cpl_census_rows, C.n_resume_cpl_census_terms,
                  rel_p50, rel_p90, rel_max, rhs_p50, rhs_max,
                  C.n_resume_cpl_census_unmatched,
                  C.n_resume_cpl_rows == 0
                      ? "  ⚠ 0 rows under an ARMED coupling restore — the snapshot "
                        "carried no QS/QL records (a pre-v3 writer, or a writer "
                        "without network.state_snapshot_coupling_capture), or no warm "
                        "resume happened: every restored leveling row injected as a "
                        "plain DIAGONAL prior, which IS the §18.11 defect."
                  : (C.n_resume_cpl_seam_present * 10
                         < (C.n_resume_cpl_seam_present
                            + C.n_resume_cpl_seam_departed) * 9)
                      ? "  ⚠ under 90 % of the restored terms resolved to a live column "
                        "at the first warm epoch — the rows were SOFTENED by "
                        "Sum M^2*P_prev, which is rung 3's mechanism by another route; "
                        "grade the h12 consumer before trusting the clock result."
                      : "");
    if (config_.networkResumeCouplingRestore())
        log->info("Network (resume coupling restore, dense border): {} dense-carry key(s) "
                  "restored from the snapshot's QK/QP records ({} dropped: no restored D "
                  "row); {} border key(s) folded DIAGONALLY at the first warm fold.",
                  C.n_resume_cpl_cov_keys, C.n_resume_cpl_cov_dropped,
                  C.n_resume_cpl_diag_border);
    // Run-end readout of the dense-border restore, and of the dense border on a plain
    // warm resume that arms neither restore.  Both edges of one instrument: how much of
    // the border came back, and how much of it the first fold still took diagonally.
    // The clock-anchor first-fold decouple count is the trip statistic of a diagonal
    // border, the number the restored border should bring back to a continuous run's
    // level.  It is structurally 0 on a lineage that does not arm
    // network.clock_temporal_anchor, and the line says so rather than letting a 0 read
    // as a good result.
    {
        const std::string cta_note =
            ctx.knobs.cta_on
                ? std::string{}
                : std::string("  (network.clock_anchor is OFF on this lineage — that 0 "
                              "is structural, not a health signal.)");
        if (config_.networkResumeBorderRestore())
            log->info("Network (RS-7 border restore, dense border): {} dense-carry key(s) "
                      "restored from the snapshot's QK/QP records ({} dropped: no "
                      "restored D row) against {} carried key(s) at the seam; {} border "
                      "key(s) STILL folded DIAGONALLY at the first warm fold.  First "
                      "post-resume epoch: {} Q3 clock-anchor decouple(s).{}{}",
                      C.n_resume_cpl_cov_keys, C.n_resume_cpl_cov_dropped,
                      C.n_resume_dense_carry_keys, C.n_resume_cpl_diag_border,
                      C.n_resume_first_fold_cta_trips, cta_note,
                      C.n_resume_cpl_cov_keys == 0
                          ? "  ⚠ 0 keys restored under an ARMED border restore — the "
                            "snapshot carried no usable QK/QP records (a writer without "
                            "network.state_snapshot_coupling_capture), or no warm resume "
                            "happened; the border re-seeded from the first emitted epoch "
                            "exactly as before."
                          : "");
        else if (C.n_resume_dense_carry_keys > 0
                 && !config_.networkResumeCouplingRestore())
            log->info("Network (C1 dense border, warm resume): {} carried key(s) came "
                      "back with VALUES and per-key variances only — {} of them folded "
                      "DIAGONALLY at the first warm fold, because neither "
                      "network.resume_border_restore nor network.resume_coupling_restore "
                      "is armed, so the block's cross-covariance re-seeds from the first "
                      "emitted epoch.  First post-resume epoch: {} Q3 clock-anchor "
                      "decouple(s).{}",
                      C.n_resume_dense_carry_keys, C.n_resume_cpl_diag_border,
                      C.n_resume_first_fold_cta_trips, cta_note);
    }
    return 0;
}

/**
 * @brief Run-end section: resume and snapshot: resume event, snapshot write, per-layer restores, coupling capture.
 *
 * The resume and state-snapshot events: the resume event, the snapshot write, the
 * linearization-frame carry, the datum, emit, station, canonical-signal and code-bias
 * restores, the phase-bias seam mark and the coupling capture. The canonical-signal
 * audit is summed into locals.
 *
 * Gate: each line under its own condition: `carry_invariant &&
 * !config_.getNetworkConfig().state_snapshot_path.empty()`;
 * `config_.getNetworkConfig().state_snapshot_datum_capture ||
 * config_.networkResumeDatumRestore()`;
 * `config_.getNetworkConfig().state_snapshot_emit_capture ||
 * config_.networkResumeEmitRestore()`;
 * `config_.getNetworkConfig().state_snapshot_station_capture ||
 * config_.networkResumeStationRestore()`;
 * `config_.getNetworkConfig().state_snapshot_signal_capture ||
 * config_.networkResumeSignalCanonRestore()`;
 * `config_.getNetworkConfig().state_snapshot_rxcb_lin_capture ||
 * config_.networkResumeRxCbLinRestore()`; `config_.networkResumePbSeamMark()`;
 * `config_.getNetworkConfig().state_snapshot_coupling_capture`.
 *
 * @param v The run at its end (network/runend/RunEnd.hpp).
 * @return 0 (this report does not set the exit code).
 */
static int networkResumeSnapshotRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const GnssConfig& config_ = v.config;
    const std::shared_ptr<spdlog::logger>& log = v.log;
    const NetworkSetupResult& setup = v.setup;
    const NetworkInvariantCarryDriver& carry_driver = v.run.engines.carry_driver;
    const bool carry_invariant = v.ctx.knobs.carry_invariant;

    // Resume event readout: its wiring is visible only through counters, so "0 restored"
    // is stated rather than inferred from absence.
    if (carry_invariant && !config_.getNetworkConfig().state_snapshot_path.empty()) {
        // Provenance of the σ̂₀ seed.  Four distinct conditions suppress the seed (no
        // carry restored, calibration off, no 'S,' line, a rejected 'S,' line); the driver
        // reports which one held, and the line names it rather than a generic cause.
        using Seed = NetworkInvariantCarryDriver::ResumeSeedState;
        const auto seed_st = carry_driver.resumeSigma0Status();
        std::string sigma0_txt;
        switch (seed_st.state) {
            case Seed::Seeded:
                sigma0_txt = fmt::format("SEEDED {:.4f} from the blob's 'S,' line ({:.0f} "
                                         "row(s)) — the D5 gate trusts the restored "
                                         "inflation immediately",
                                         seed_st.sigma0, seed_st.blob_rows);
                break;
            case Seed::NotRestored:
                sigma0_txt = "NOT seeded — COLD: no carry blob was restored this run, so "
                             "there was no σ̂₀ to seed from; the gate runs raw-formal until "
                             "its warm-up count is met";
                break;
            case Seed::CalibrationOff:
                sigma0_txt = "NOT seeded — network.carry_invariant_innov_calibrate is OFF, "
                             "so no σ̂₀² accumulator exists and any 'S,' line in the blob "
                             "was never consulted; the gate is raw-formal BY CONFIG, not "
                             "pending a warm-up count";
                break;
            case Seed::NoSeedLine:
                sigma0_txt = "NOT seeded — the restored blob carried no 'S,' line (a v1/"
                             "legacy blob, or one written by a calibration-off run); the "
                             "gate runs raw-formal until its warm-up count is met";
                break;
            case Seed::Rejected:
                sigma0_txt = fmt::format(
                    "NOT seeded — the blob's 'S,' line was REJECTED (σ̂₀² = {:.4g}, rows = "
                    "{:.0f}; the seed guard needs both strictly positive).  That is a "
                    "WRITER-side defect, not a legacy blob — the run that wrote this "
                    "snapshot serialized a degenerate accumulator; the gate runs raw-formal "
                    "until its warm-up count is met",
                    seed_st.blob_sigma0_sq, seed_st.blob_rows);
                break;
        }
        log->info("Network (RTB-7d resume event): carry restored {}{}; sigma0 [{}] {}; "
                  "restored Pᶻ {}.",
                  carry_driver.resumeCarryRestored() ? "YES" : "NO",
                  carry_driver.resumeCarryRestored()
                      ? fmt::format(" ({} carried DD at restore)",
                                    carry_driver.resumeCarryChords())
                      : std::string{},
                  NetworkInvariantCarryDriver::resumeSeedStateName(seed_st.state), sigma0_txt,
                  // The shape that was restored.  Without it the diagonal arm of the
                  // A/B looks the same as the dense one in every log line and counter.
                  carry_driver.resumeCarryDiagonal()
                      ? fmt::format("DIAGONAL-ONLY (v3, {} block(s) — cross-covariances "
                                    "started at zero)",
                                    carry_driver.resumeCarryDiagonalBlocks())
                      : std::string("dense (v1/v2) or nothing restored"));
        // Write side: the blob is about 99 % of the snapshot at scale, so its size is the
        // readout that the diagonal knob took effect.
        if (config_.getNetworkConfig().state_snapshot_carry)
            log->info("Network (RTB-7e snapshot write): {} snapshot(s) written; last carry "
                      "blob {} bytes ({:.2f} MB), shape {}.",
                      ctx.counters.snap_writes, ctx.counters.snap_carry_bytes_last,
                      static_cast<double>(ctx.counters.snap_carry_bytes_last) / (1024.0 * 1024.0),
                      ctx.counters.snap_carry_diagonal
                          ? "DIAGONAL-ONLY (v3 — Sum K_c numbers)"
                          : "dense (v2 — Sum K_c^2 numbers)");
        // Linearization-frame carry, the write half.  The restored ZL-row count shows
        // whether a resumed blob carried the rows.
        if (config_.getNetworkConfig().state_snapshot_carry_lin)
            log->info("Network (RS-2 linearization carry): write ARMED; blob carried ZL rows "
                      "on {} restored block(s).",
                      carry_driver.resumeCarryLinBlocks());
    }
    // Datum-continuity halves.  At function scope, like the coupling-capture readout,
    // and outside the carry_invariant-gated resume-event block above: the eq34 chain
    // datum and the time-linked code-bias datum run on lineages with and without the
    // invariant branch, so nesting the line there would leave it structurally absent on
    // half the postures that arm it.  Printed whenever either half is armed: an armed
    // writer that wrote no rows and an armed reader that restored none are the two cases
    // this line exposes, and the bare "off" on the other half says which one it is.  The
    // first-post-resume-epoch figures are counted on every warm resume regardless, so an
    // A/B control leg reports them too.
    if (config_.getNetworkConfig().state_snapshot_datum_capture
        || config_.networkResumeDatumRestore()) {
        const auto& C = ctx.counters;
        const long long n_eq34_restored =
            C.n_resume_datum_eq34_rx + C.n_resume_datum_eq34_edges
            + C.n_resume_datum_sat_roots + C.n_resume_datum_prev_targets
            + C.n_resume_datum_prev_el + C.n_resume_datum_retained;
        log->info("Network (RS-3 datum restore): write {} / read {}; last snapshot carried "
                  "{} datum row(s); restored eq34 datum rx {}, rx parents {}, sat roots {}, "
                  "prev targets {} (+{} longevity elevation(s), {} gap-retained edge(s)); "
                  "CB prev {}, seen {}, IF-pin pairs {}; first post-resume epoch: eq34 "
                  "rerefs {}, reattaches {}, CB re-seed discs {}.{}",
                  config_.getNetworkConfig().state_snapshot_datum_capture ? "ARMED" : "off",
                  config_.networkResumeDatumRestore() ? "ARMED" : "off",
                  ctx.counters.snap_datum_rows_last,
                  C.n_resume_datum_eq34_rx, C.n_resume_datum_eq34_edges,
                  C.n_resume_datum_sat_roots, C.n_resume_datum_prev_targets,
                  C.n_resume_datum_prev_el, C.n_resume_datum_retained,
                  C.n_resume_datum_cb_prev, C.n_resume_datum_cb_seen,
                  C.n_resume_datum_cb_pair,
                  C.n_resume_datum_first_reref, C.n_resume_datum_first_reattach,
                  C.n_resume_datum_first_cb_disc,
                  (config_.networkResumeDatumRestore() && n_eq34_restored == 0
                   && C.n_resume_datum_cb_prev == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no "
                        "EQ* / CB* rows (a writer without "
                        "network.state_snapshot_datum_capture?) or no warm resume "
                        "happened; check the resume log for the RS-3 line."
                      : "");
    }
    // Emit-layer continuity halves, beside the datum line and at the same function scope
    // for the same reason: the re-base runs on lineages with and without the invariant
    // branch.  Printed whenever either half is armed: an armed writer that wrote no rows
    // and an armed reader that restored none are the two cases this line exposes, and the
    // bare "off" on the other half says which one it is.
    if (config_.getNetworkConfig().state_snapshot_emit_capture
        || config_.networkResumeEmitRestore()) {
        const auto& C = ctx.counters;
        log->info("Network (RS-5 emit restore): write {} / read {}; last snapshot carried "
                  "{} emit-layer row(s); restored re-base keys {}.{}",
                  config_.getNetworkConfig().state_snapshot_emit_capture ? "ARMED" : "off",
                  config_.networkResumeEmitRestore() ? "ARMED" : "off",
                  ctx.counters.snap_emit_rows_last,
                  C.n_resume_emit_rebase_keys,
                  (config_.networkResumeEmitRestore()
                   && C.n_resume_emit_rebase_keys == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no "
                        "ER rows (a writer without "
                        "network.state_snapshot_emit_capture?), the emit re-base is not "
                        "running this run, or no warm resume happened; check the resume "
                        "log for the RS-5 line."
                      : "");
    }
    // Station-layer continuity halves, beside the emit line and at the same function
    // scope for the same reason: the two carries run on lineages that do and do not arm
    // each other.  Printed whenever either half is armed: an armed writer that wrote no
    // rows and an armed reader that restored none are the two cases this line exposes,
    // and the bare "off" on the other half says which one it is.  The rx-CB sub-switch
    // state is always named, so an attribution leg that restored one half can be read
    // from the run-end tail alone.
    if (config_.getNetworkConfig().state_snapshot_station_capture
        || config_.networkResumeStationRestore()) {
        const auto& C = ctx.counters;
        log->info("Network (RS-6 station restore): write {} / read {} (rxcb {}); "
                  "last snapshot carried {} station-layer row(s); restored rx-CB entries "
                  "{}, deviation entries {}.{}",
                  config_.getNetworkConfig().state_snapshot_station_capture ? "ARMED" : "off",
                  config_.networkResumeStationRestore() ? "ARMED" : "off",
                  config_.networkResumeStationRestoreRxCb() ? "on" : "off",
                  ctx.counters.snap_station_rows_last,
                  C.n_resume_station_rxcb, C.n_resume_station_shdev,
                  (config_.networkResumeStationRestore()
                   && C.n_resume_station_rxcb == 0 && C.n_resume_station_shdev == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no "
                        "RC / SD rows (a writer without "
                        "network.state_snapshot_station_capture?), neither carry is armed "
                        "this run, or no warm resume happened; check the resume log for "
                        "the RS-6 line."
                      : "");
    }
    // Canonical-signal latch, beside the station line and printed only when armed, as
    // that line is.  The disagree count reads in both directions: > 0 is the defect being
    // prevented (that many restored entries would have latched differently on this run's
    // first epoch), and 0 says the seam epoch happened to agree.  It never says the
    // wiring did nothing; the stations/entries pair reports that.
    if (config_.getNetworkConfig().state_snapshot_signal_capture
        || config_.networkResumeSignalCanonRestore()) {
        const auto& C = ctx.counters;
        // The audit runs inside the station models on the first post-resume epoch, so
        // the roster is the only place the result lives; it is summed here.
        long long audited = 0, disagree = 0;
        for (const auto& st : setup.stations) {
            if (!st.model) continue;
            audited  += st.model->canonicalAuditEntries();
            disagree += st.model->canonicalAuditDisagreements();
        }
        log->info("Network (RS-8 canonical restore): write {} / read {}; last snapshot "
                  "carried {} CN row(s); restored {} entr{} across {} station(s); first "
                  "post-resume epoch audited {} of them and {} would have been decided "
                  "DIFFERENTLY here.{}",
                  config_.getNetworkConfig().state_snapshot_signal_capture ? "ARMED" : "off",
                  config_.networkResumeSignalCanonRestore() ? "ARMED" : "off",
                  ctx.counters.snap_canon_rows_last,
                  C.n_resume_canon_entries, C.n_resume_canon_entries == 1 ? "y" : "ies",
                  C.n_resume_canon_stations, audited, disagree,
                  (config_.networkResumeSignalCanonRestore()
                   && C.n_resume_canon_entries == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no CN "
                        "rows (a writer without network.state_snapshot_signal_capture?) "
                        "or no warm resume happened; check the resume log for the RS-8 "
                        "line."
                      : (disagree > 0
                             ? "  Those are the flips the restore prevents: without the "
                               "restore each of them re-decides the station's canonical "
                               "per-band channel at the seam."
                             : ""));
    }
    // Code-bias linearization-point half, beside the station line: that one restores the
    // rx-CB carry's numerator and this restores the reference its inject gate differences
    // it against, and a run that restored one without the other has to be readable from
    // the run-end tail alone.  Printed whenever either half is armed.
    if (config_.getNetworkConfig().state_snapshot_rxcb_lin_capture
        || config_.networkResumeRxCbLinRestore()) {
        const auto& C = ctx.counters;
        // The applied / uncovered tallies are written station-locally inside the
        // parallel produce phase (one out-slot per station, never resized) and summed
        // here, the only place that needs them, which is serial by construction.
        long long applied = 0, uncovered = 0;
        for (const auto& [a, u] : ctx.carry.rxcb_lin_sta_counts) { applied += a; uncovered += u; }
        // "uncovered" is scoped to the carried keys (the RL rows pair 1:1 with the RC
        // rows): a code-bias family with no carry never reaches the gate and is not a
        // miss, so a nonzero here is a carried key whose reference could not be applied.
        log->info("Network (RS-9 rx-CB frame restore): write {} / read {}; last snapshot "
                  "carried {} RL row(s); restored {} linearization point(s) across {} "
                  "station(s), seeded {}, carried-but-unseeded {}.{}",
                  config_.getNetworkConfig().state_snapshot_rxcb_lin_capture ? "ARMED" : "off",
                  config_.networkResumeRxCbLinRestore() ? "ARMED" : "off",
                  ctx.counters.snap_rxcb_lin_rows_last,
                  C.n_resume_rxcb_lin_entries, C.n_resume_rxcb_lin_stations,
                  applied, uncovered,
                  (config_.networkResumeRxCbLinRestore() && C.n_resume_rxcb_lin_entries == 0)
                      ? "  ⚠ 0 restored under an ARMED read — the snapshot carried no RL "
                        "rows (a writer without network.state_snapshot_rxcb_lin_capture?) "
                        "or no warm resume happened; check the resume log for the RS-9 "
                        "line."
                      : ((config_.networkResumeRxCbLinRestore()
                          && C.n_resume_rxcb_lin_entries > 0 && applied == 0)
                             ? "  ⚠ points were STASHED and none was ever SEEDED — no "
                               "station reached its first-epoch code-bias initialization "
                               "with a carried key (estimate_ifb off, or the samples loop "
                               "never saw those families), so the gate still reads this "
                               "leg's own frame."
                             : (uncovered > 0
                                    ? "  ⚠ NONZERO carried-but-unseeded — a key WITH an "
                                      "rx-CB carry could not take its restored reference "
                                      "(non-finite value, or a code-bias state index this "
                                      "run does not hold), so the gate reads this leg's "
                                      "own frame for it and can refuse that station for "
                                      "the whole leg.  A code-bias family with no carry is "
                                      "NOT counted here."
                                    : "")));
    }
    // Phase-bias seam mark (the declaration half), beside the datum line and at the same
    // function scope for the same reason.  The census is taken on every warm resume the
    // knob is armed for, so an armed run that marked nothing reports its tested/over
    // counts and an armed run that never resumed reports zeros; the line tells those two
    // cases apart.  Printed only when the knob is armed: with it off nothing is measured
    // (pbSeamCensus is never called), and a 0/0 line would be a false instrument.
    if (config_.networkResumePbSeamMark()) {
        const auto& C = ctx.counters;
        const double pct = C.n_pb_seam_keys_tested > 0
                               ? 100.0 * static_cast<double>(C.n_pb_seam_keys_over)
                                     / static_cast<double>(C.n_pb_seam_keys_tested)
                               : 0.0;
        log->info("Network (RS-4 PB seam mark): ARMED; keys tested {}, over threshold {} "
                  "({:.1f} %), constellations marked {} (bar {:.0f} %).{}",
                  C.n_pb_seam_keys_tested, C.n_pb_seam_keys_over, pct, C.n_pb_seam_marks,
                  GnssConfig::kResumePbSeamMarkFraction * 100.0,
                  C.n_pb_seam_keys_tested == 0
                      ? "  ⚠ 0 keys tested — no warm resume happened this run, or the "
                        "first post-resume emit found no restored emitter baselines (a "
                        "snapshot without an EMITTER blob); the rule is INERT here."
                      : "");
    }
    // Coupling capture, write side: the sibling of the snapshot-write readout above, but
    // at function scope on purpose.  The resume-event block above is gated on
    // carry_invariant, while the coupling capture rides the phase-leveling carries
    // (carry_conditional_coupling; carry_invariant supersedes the leveling local carry,
    // and the reference configs run carry_invariant: false), so nesting it there would
    // leave the line structurally absent on the postures that arm it.  About 25 B per
    // (id, coeff) pair is the approximate size of a QL/QZ entry; it turns "rows
    // captured" into the disk and I/O size a keep-history leg has to plan for.
    if (config_.getNetworkConfig().state_snapshot_coupling_capture)
        log->info("Network (§18.12 coupling capture): last snapshot carried {} L row(s) "
                  "+ {} Z row(s), {} regressor term(s) over {} shared column(s) "
                  "(~{:.2f} MB of QL/QZ payload); {} row(s) omitted whole for a "
                  "non-finite field over the run.{}",
                  ctx.counters.snap_cpl_rows_last, ctx.counters.snap_cpl_zrows_last,
                  ctx.counters.snap_cpl_terms_last, ctx.counters.snap_cpl_dict_last,
                  static_cast<double>(ctx.counters.snap_cpl_terms_last) * 25.0
                      / (1024.0 * 1024.0),
                  ctx.counters.snap_cpl_rows_dropped,
                  ctx.counters.snap_cpl_rows_last == 0
                      ? "  ⚠ 0 rows under an ARMED capture — carry_conditional_coupling "
                        "off, or every carry harvested EMPTY: the file is v1/v2 and no "
                        "resume can use it."
                      : "");
    return 0;
}

// Run-end report of this subsystem: its sections in print order (declared in
// network/runend/ResumeRunEnd.hpp).
int networkResumeRunEnd(const NetworkRunEndView& v) {
    return networkRunEndSections(v, {
        networkResumeRestoreRunEnd,
        networkResumeSnapshotRunEnd
    });
}

// ---- End of the run-end reports ----

} // namespace GnssProcessor
