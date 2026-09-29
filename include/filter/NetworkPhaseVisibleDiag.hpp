/**
 * @file NetworkPhaseVisibleDiag.hpp
 * @brief The phase-visible consistency instrument (card PVN,
 *        docs/refound/PVN_phase_visible_nees_2026-09-29.md), a measure-only diagnostic seam.
 *
 * Card LAH section 8: the smoothed phase-bias hold fails because the estimator states the
 * ionosphere-free phase-visible clock of each satellite, @f$D = c - \sum_j w_j p_j@f$ (one
 * phase bias per band, @f$\sum w = 1@f$, @f$\sum w\lambda^2 = 0@f$), to a few millimetres
 * while it wanders centimetres within an arc.  This seam measures that consistency every
 * epoch, on the functional and the constellation contrast of the anchor guard's phase
 * statistic (@ref clockAnchorPhaseStatistic, card CTU2 section 5), read without its floor:
 *
 *  - the normalized update @f$e^2 / (S^- - S^+)@f$, where @f$e@f$ is the contrast update of
 *    @f$D@f$ against the dense fold's prior and @f$S^\mp@f$ its contrast-projected prior and
 *    posterior variances.  For a consistent filter every functional's update has variance
 *    @f$g^\top(P^- - P^+)g@f$, so the mean is 1;
 *  - the posterior contrast value and variance, from which the sim's within-arc state NEES
 *    is graded offline (the sim's true @f$D@f$ is constant within a phase-datum era).
 *
 * Seam contract: a free function over the run context that owns no state; the open window
 * lives in @c ctx.carry, the run totals in @c ctx.counters.  The solve and every product are
 * untouched; with the knob off the seam returns before reading anything.
 */
#ifndef GNSSPROCESSOR_NETWORK_PHASE_VISIBLE_DIAG_HPP
#define GNSSPROCESSOR_NETWORK_PHASE_VISIBLE_DIAG_HPP

#include <Eigen/Dense>

namespace GnssProcessor {

class NetworkEstimator;
class NetworkRunTraces;
struct EpochWorkspace;
struct NetworkRunContext;
struct SyncedEpoch;

/// The normalized update of one functional (card PVN): @f$e^2 / (S^- - S^+)@f$.
/// @param e_hat  the contrast update (m).
/// @param s_m    the contrast-projected prior variance @f$S^-@f$ (m^2).
/// @param s_p    the contrast-projected posterior variance @f$S^+@f$ (m^2).
/// @param nis    out: the NIS; untouched when the result is false.
/// @return false when @f$S^- - S^+@f$ is not positive or an input is not finite.
bool phaseVisibleNis(double e_hat, double s_m, double s_p, double& nis);

/**
 * @brief Run the phase-visible instrument on one kept epoch (card PVN).
 *
 * Forms the anchor guard's phase statistic with no floor from the epoch's dense fold
 * capture (@c ws.cap_dense_keys / @c ws.cap_dense_cov, the prior) and the solve
 * (@p est's shared covariance and @p x_s, the posterior), writes one trace row per
 * satellite, accumulates the NIS after the 15th epoch into the run totals and the open
 * window, and closes the window every @c knobs.pv_window_ep epochs (its mean NIS and the
 * ratio of the rms update to the rms predicted update sd).  An epoch without a capture is
 * counted and writes nothing.
 *
 * @param ctx     Run context: the knob snapshot, the open window, the counters.
 * @param traces  Trace sinks (@c phase_visible_trace).
 * @param se      The synced epoch (its time stamps the rows).
 * @param est     The estimator after the epoch's final solve.
 * @param x_s     The shared solution of that solve.
 * @param ws      The epoch workspace holding the fold capture.
 *
 * @note Returns immediately when @c ctx.knobs.pv_on is false.
 */
void networkPhaseVisibleMonitor(NetworkRunContext& ctx, NetworkRunTraces& traces,
                                const SyncedEpoch& se, const NetworkEstimator& est,
                                const Eigen::VectorXd& x_s, const EpochWorkspace& ws);

}  // namespace GnssProcessor

#endif  // GNSSPROCESSOR_NETWORK_PHASE_VISIBLE_DIAG_HPP
