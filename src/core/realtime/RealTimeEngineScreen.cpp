/**
 * @file RealTimeEngineScreen.cpp
 * @brief The consumer's pre-update innovation screen: builds the epoch's first
 *        measurement pass, tests it against the predicted state, holds flagged
 *        rows out of the update and re-floats persistent phase rows.
 *
 * The statistic, the sequential removal and the exclusion episodes are in
 * InnovationScreen (pure, unit-tested on synthetic matrices). This file is the
 * engine side: it forms the innovation covariance from the state manager's
 * predicted covariance and the measurement's design matrix, maps the decisions
 * onto the measurement model's exclusion keys and the state manager's reset path,
 * and writes the events (log, consumer satellite trace) and the run-end summary.
 *
 * Placement. The screen runs inside runMeasurementUpdate, after the predict, the
 * slip detection and the datum-break reactions have settled the predicted state,
 * and before the first update pass. That is the only point at which the innovations
 * are still free of the error a gross row would spread through the update.
 */

#include "realtime/RealTimeEngine.hpp"
#include "filter/StateKeySchema.hpp"    // makeAmbiguityKey
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace GnssProcessor {

namespace {

/// True for an undifferenced single-satellite code or phase row ("G32", "L1W"), of the
/// class the screen was asked to cover. Differenced rows ("<sat>-<pivot>"), aiding rows
/// and combinations carry labels that fail this and take no part in the screen.
bool isScreenableRow(const std::string& sat, const std::string& signal,
                     InnovationScreenRows rows) {
    if (sat.size() != 3 || !std::isalpha(static_cast<unsigned char>(sat[0]))
        || !std::isdigit(static_cast<unsigned char>(sat[1]))
        || !std::isdigit(static_cast<unsigned char>(sat[2])))
        return false;
    if (signal.size() != 3 || !std::isdigit(static_cast<unsigned char>(signal[1])))
        return false;
    const bool phase = (signal[0] == 'L');
    const bool code  = (signal[0] == 'C');
    switch (rows) {
    case InnovationScreenRows::All:   return phase || code;
    case InnovationScreenRows::Phase: return phase;
    case InnovationScreenRows::Code:  return code;
    }
    return false;
}

/// The name an event takes on the consumer satellite trace.
const char* traceName(InnovationScreen::EventKind kind, bool acted) {
    using K = InnovationScreen::EventKind;
    switch (kind) {
    case K::Exclude:  return acted ? "screen_excl"     : "screen_obs_excl";
    case K::Readmit:  return acted ? "screen_readmit"  : "screen_obs_readmit";
    case K::Refloat:  return acted ? "screen_refloat"  : "screen_obs_refloat";
    case K::Lost:     return acted ? "screen_lost"     : "screen_obs_lost";
    case K::Released: return acted ? "screen_released" : "screen_obs_released";
    }
    return "screen_excl";
}

} // namespace

void RealTimeEngine::configureInnovationScreen_() {
    // Resolve the knobs once (the config cannot change mid-run).
    if (innovation_screen_configured_) return;
    InnovationScreen::Config c;
    c.k = config_.getInnovationScreenK();
    c.mode = (config_.getInnovationScreenMode() == "observe")
                 ? InnovationScreen::Mode::Observe : InnovationScreen::Mode::Act;
    const std::string& rows = config_.getInnovationScreenRows();
    c.rows = (rows == "phase") ? InnovationScreenRows::Phase
           : (rows == "code")  ? InnovationScreenRows::Code
                               : InnovationScreenRows::All;
    c.max_fraction = config_.getInnovationScreenMaxFraction();
    c.refloat_after_ep = config_.getInnovationScreenRefloatEp();
    innovation_screen_.configure(c);
    innovation_screen_configured_ = true;
}

bool RealTimeEngine::screenInnovations_(RtEpochContext& ctx, std::set<std::string>& excluded) {
    if (!config_.innovationScreen()) return false;
    configureInnovationScreen_();

    // The first pass, exactly as the update loop would build it.
    EpochMeasurement& mu = ctx.meas_update;
    mu = meas_model_->computeMeasurementUpdate(
        ctx.modeled_obs, state_manager_->getStateVector(), excluded);

    // An unusable measurement goes back to the loop, which ends the epoch on it as before.
    if (mu.innovations.size() == 0 || !meas_model_->validateMeasurementUpdate(mu))
        return true;

    const InnovationScreen::Outcome out = evaluateInnovationScreen_(mu);
    applyInnovationScreen_(out, excluded);

    // Rows were held out, or an ambiguity was re-floated (and is re-seeded when the
    // measurement is built): build the first pass again from the screened set.
    if (out.changesUpdate())
        mu = meas_model_->computeMeasurementUpdate(
            ctx.modeled_obs, state_manager_->getStateVector(), excluded);
    return true;
}

InnovationScreen::Outcome RealTimeEngine::evaluateInnovationScreen_(const EpochMeasurement& mu) {
    const Eigen::Index m = mu.innovations.size();
    const size_t n_lbl = std::min(mu.satellite_ids.size(), mu.signal_types.size());
    const InnovationScreenRows cls = innovation_screen_.config().rows;

    std::vector<InnovationScreen::Row> rows(n_lbl);
    for (size_t i = 0; i < n_lbl; ++i) {
        rows[i].sat = mu.satellite_ids[i];
        rows[i].signal = mu.signal_types[i];
        rows[i].eligible = isScreenableRow(rows[i].sat, rows[i].signal, cls);
    }

    // Innovation covariance of the predicted state: S = H P H^T + R, plus a model-error
    // floor per row class. The formal noise is millimetres on phase, while the
    // epoch-to-epoch innovations of a healthy consumer spread over decimetres (product
    // clock and orbit error, ionosphere, multipath); without the floor the statistic
    // flags ordinary rows. The filter's own robust inflations are applied later, inside
    // the update, and are not in S. An inconsistent size leaves S empty, which the
    // screen counts as a skipped call.
    Eigen::MatrixXd S;
    const Eigen::MatrixXd P = state_manager_->getCovariance();
    if (mu.H.rows() == m && mu.H.cols() == P.rows() && P.rows() == P.cols()) {
        S.noalias() = (mu.H * P) * mu.H.transpose();
        if (mu.R_full.rows() == m && mu.R_full.cols() == m)
            S += mu.R_full;
        else if (mu.R_v.size() == m)
            S.diagonal() += mu.R_v;
        const double fp = config_.getInnovationScreenPhaseFloorM();
        const double fc = config_.getInnovationScreenCodeFloorM();
        for (Eigen::Index i = 0; i < m && static_cast<size_t>(i) < n_lbl; ++i) {
            const std::string& sig = rows[static_cast<size_t>(i)].signal;
            if (sig.empty()) continue;
            if (sig[0] == 'L')      S(i, i) += fp * fp;
            else if (sig[0] == 'C') S(i, i) += fc * fc;
        }
    }
    return innovation_screen_.evaluate(S, mu.innovations, rows);
}

void RealTimeEngine::applyInnovationScreen_(const InnovationScreen::Outcome& out,
                                            std::set<std::string>& excluded) {
    using K = InnovationScreen::EventKind;
    auto log = log_manager_.getLogger("rt_engine");
    const std::string epoch_prefix = "[RT] ";
    const bool act = out.acted;
    const char* mode = act ? "" : " (observe: nothing written)";

    // Every event of the epoch, with its identity, on the log and the satellite trace.
    for (const auto& e : out.events) {
        switch (e.kind) {
        case K::Exclude:
            log->info("{}Innovation screen: EXCLUDE {} {} w={:+.1f} (conditional sigma "
                      "{:.3f} m, innovation {:+.3f} m), flagged on {} consecutive epoch(s){}",
                      epoch_prefix, e.sat, e.signal, e.w, e.sigma, e.innovation, e.streak, mode);
            break;
        case K::Refloat:
            log->info("{}Innovation screen: REFLOAT {} {} w={:+.1f} (conditional sigma "
                      "{:.3f} m, innovation {:+.3f} m), flagged on {} consecutive epoch(s): the "
                      "ambiguity is re-floated and the row returns to the update{}",
                      epoch_prefix, e.sat, e.signal, e.w, e.sigma, e.innovation, e.streak, mode);
            break;
        case K::Readmit:
            log->info("{}Innovation screen: READMIT {} {} passes again after {} flagged "
                      "epoch(s){}", epoch_prefix, e.sat, e.signal, e.streak, mode);
            break;
        case K::Lost:
            log->info("{}Innovation screen: LOST {} {} left the row set while excluded "
                      "({} flagged epoch(s)){}", epoch_prefix, e.sat, e.signal, e.streak, mode);
            break;
        case K::Released:
            log->info("{}Innovation screen: RELEASED {} {} (the screen abstained, the row "
                      "is in the update; {} flagged epoch(s)){}",
                      epoch_prefix, e.sat, e.signal, e.streak, mode);
            break;
        }
        noteConsumerSatEvent_(e.sat + "|" + e.signal, traceName(e.kind, act));
    }
    if (out.abstain != InnovationScreen::Abstain::None)
        log->info("{}Innovation screen: ABSTAIN ({}), {} row(s) flagged before it gave up; "
                  "the epoch goes to the update unscreened, largest |w| {:.1f}",
                  epoch_prefix, InnovationScreen::abstainName(out.abstain),
                  out.flagged_before_abstain, out.max_abs_w);

    if (!act) return;

    // The rows held out: the measurement model reads these keys when it builds the pass.
    for (const auto& key : out.exclude) excluded.insert(key);

    // The re-float of a persistent phase row: the first-sighting reset path, then the
    // detectors that keep an arc forget it (a stale arc would flag against the new
    // ambiguity at the next epoch).
    const double reset_std = config_.getAmbiguityStd();
    for (const auto& [sat, sig] : out.refloat) {
        const bool ok = state_manager_->resetAmbiguity(StateKeys::makeAmbiguityKey(sat, sig),
                                                       reset_std);
        noteAmbLevelDisturbed_(sat);
        if (mw_slip_detector_) mw_slip_detector_->resetSatelliteSignal(sat, sig);
        if (turbo_edit_resolver_) turbo_edit_resolver_->resetSatelliteSignal(sat, sig);
        log->info("{}Innovation screen: re-floated {} {} at std {:.3f} m ({})",
                  epoch_prefix, sat, sig, reset_std, ok ? "ok" : "no ambiguity state");
    }
}

void RealTimeEngine::logInnovationScreenSummary(const char* tag) const {
    if (!config_.innovationScreen()) return;   // knob off: never printed (byte-identical)
    const auto& c = innovation_screen_.counters();
    auto log = log_manager_.getLogger("rt_engine");
    // The echo reads the config, not the screen: with no epoch processed the screen has
    // not been configured yet.
    log->info("[RT] Consumer innovation screen ({}, mode '{}', rows '{}', k {:.2f}, max fraction "
              "{:.2f}, re-float after {} ep): epochs {} / eligible rows {} -> flagged {} "
              "(phase {} / code {}) in {} epoch(s), largest |w| {:.1f}, longest streak {}; "
              "abstained {} (too many {} / too few {} / numerical {}); skipped calls {}",
              tag, config_.getInnovationScreenMode(), config_.getInnovationScreenRows(),
              config_.getInnovationScreenK(), config_.getInnovationScreenMaxFraction(),
              config_.getInnovationScreenRefloatEp(), c.epochs, c.rows,
              c.flagged, c.flagged_phase, c.flagged_code, c.epochs_flagged, c.max_abs_w,
              c.max_streak, c.abstained(), c.abstain_many, c.abstain_few, c.abstain_numeric,
              c.skipped);
    log->info("[RT] Consumer innovation screen ({}): exclusion episodes {} -> readmitted {} / "
              "re-floated {} / lost {} / released by an abstaining epoch {} / still open {}; "
              "closure {}",
              tag, c.episodes, c.readmitted, c.refloated, c.lost, c.released, c.open(),
              c.closureHolds() ? "HOLDS" : "BROKEN");
    log->info("[RT] Consumer innovation screen ({}): epochs by their largest |w| over the "
              "eligible rows: <2 {} / 2-3 {} / 3-4 {} / 4-5 {} / 5-6 {} / 6-10 {} / 10-100 {} / "
              ">=100 {}",
              tag, c.hist[0], c.hist[1], c.hist[2], c.hist[3], c.hist[4], c.hist[5], c.hist[6],
              c.hist[7]);
}

} // namespace GnssProcessor
