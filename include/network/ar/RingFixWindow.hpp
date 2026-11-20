#pragma once
/**
 * @file RingFixWindow.hpp
 * @brief The windowed residual monitor of held ring-fix links (network.ring_fix.window_monitor; card NRF section
 *        132): measure only, nothing released, no product changed.
 *
 * For every held link with a post-fit ionosphere-free residual r(t) the monitor forms, each epoch:
 *   - the scaled sums S+(t) = max(0, S+(t-1) + r/s - d/s), S-(t) = max(0, S-(t-1) - r/s - d/s) with two class
 *     choices: the station class (the link's station, system and signal pair) and the network class (the station's
 *     receiver class, system and pair).  s is the robust scale (1.4826 times the MAD about the median) of the other
 *     links' residuals of the class over the trailing hour; the link's own residuals never enter it.  d is half the
 *     residual a one-cycle (1, 1) error leaves on a held link of the pair.  While any link of the class is in an
 *     episode of the form, each link's scale stays at its last value.  With fewer than kRingFixWindowMinOthers other
 *     links behind the class, the sums are not formed and the link-epoch is counted unscaled (no floor);
 *   - the paper's fixed forms O2b, O1 and O2a beside them (today's 30 mm rule is unchanged, in the decision step);
 *   - share_sta and share_sat: the share of the other held links at the station (and of the satellite at other
 *     stations) whose mean residual over at least 30 min of overlap with the hold reaches 4 mm.
 * An episode of a form opens at its first trip and closes at the hold's end (the first epoch the link is not held:
 * a release, a tree entry, the run's end of the link) or at a new hold of the link.
 *
 * The residual enters at the resolution of network_ring_fix.csv (five decimals of a metre), so an offline replay of
 * that trace reproduces every episode (the stage's bar 4).  Sums start from zero before the hold's first row with a
 * residual; a held row without a residual leaves every statistic unchanged.
 */

#include "network/ar/RingFix.hpp"   // RingFixWindowMonitor, RingFixWindowForm, RingFixLinkId

#include <deque>
#include <string>
#include <tuple>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;              // network/driver/Run.hpp
struct NetworkEpoch;            // network/driver/Epoch.hpp
struct RingFixDecideReadings;   // network/ar/RingFixDecide.hpp
struct RingFixDecideAudit;      // network/ar/RingFixDecide.hpp

/// One held link as the monitor reads it in an epoch.
struct RingFixWindowObs {
    RingFixLinkId id;           ///< The link.
    long long n1{0};            ///< Its held band-1 integer.
    long long n2{0};            ///< Its held band-2 integer.
    double hold_s{-1.0};        ///< Its decision time (decided_s): another value than the watched one is a new hold.
    double lambda1{0.0};        ///< Band-1 wavelength (m).
    double lambda2{0.0};        ///< Band-2 wavelength (m).
    bool   have_r{false};       ///< A residual was formed this epoch.
    double r{0.0};              ///< The residual at the trace's resolution (m).
};

/// An episode's edge.
struct RingFixWindowEdge {
    bool        open{true};     ///< True at the open, false at the close.
    RingFixWindowForm form{RingFixWindowForm::O2b};   ///< The form.
    RingFixLinkId id;           ///< The link.
    long long   n1{0};          ///< Its held band-1 integer.
    long long   n2{0};          ///< Its held band-2 integer.
    double      value{0.0};     ///< The statistic at the edge (scaled sums in scales, O2b in m-epochs, O1 in m, O2a in epochs).
    double      scale{0.0};     ///< The scaled forms: the scale used (m); 0 otherwise.
    int         scale_n{0};     ///< The scaled forms: the other links behind it.
    double      share_sta{-1.0};   ///< share_sta (-1: no neighbour with enough overlap).
    int         n_sta{0};       ///< The station neighbours with enough overlap.
    double      share_sat{-1.0};   ///< share_sat (-1: none).
    int         n_sat{0};       ///< The satellite neighbours with enough overlap.
    double      age_s{0.0};     ///< At the open: since the decision; at the close: the episode's length (s).
    const char* cause{""};      ///< At the close: "end" (not held) or "new_hold".
};

/// One held link-epoch's row of network_ring_fix_window.csv.
struct RingFixWindowRow {
    RingFixLinkId id;           ///< The link.
    bool   have_r{false};       ///< A residual was formed.
    double r{0.0};              ///< The residual used (m).
    bool   formed[2]{false, false};   ///< Per class choice: the sums were formed this epoch.
    double scale[2]{0.0, 0.0};  ///< Per class choice: the scale used (m).
    int    scale_n[2]{0, 0};    ///< Per class choice: the other links behind it.
    double s_plus[2]{0.0, 0.0}; ///< Per class choice: S+.
    double s_minus[2]{0.0, 0.0};   ///< Per class choice: S-.
    double o2b_plus{0.0};       ///< O2b S+ (m-epochs).
    double o2b_minus{0.0};      ///< O2b S- (m-epochs).
    bool   o1_valid{false};     ///< O1's window is inside the hold and covered.
    double o1_mean{0.0};        ///< O1's window mean (m).
    int    o2a_run{0};          ///< O2a's signed run (positive: positive residuals).
    double share_sta{-1.0};     ///< share_sta (-1: none).
    int    n_sta{0};            ///< Station neighbours with enough overlap.
    double share_sat{-1.0};     ///< share_sat (-1: none).
    int    n_sat{0};            ///< Satellite neighbours with enough overlap.
};

/**
 * @brief The receiver class of each roster station: its receiver type when at least kRingFixWindowMinOthers
 *        stations of the roster share it, otherwise its manufacturer (the type's first word); "unknown" when empty.
 * @param receiver_types Per roster station: the receiver type of its file header (trimmed).
 * @return Per roster station: its class.
 */
std::vector<std::string> ringFixWindowRxClasses(const std::vector<std::string>& receiver_types);

/**
 * @brief The residual at the resolution of network_ring_fix.csv (the value its five-decimal cell reads back as).
 * @param r_m The residual (m).
 * @return The value of fmt "{:.5f}" of @p r_m read back as a double.
 */
double ringFixWindowResolution(double r_m);

/**
 * @brief The reference d of a link: half of kRingFixWindowFootprintShare of the ionosphere-free footprint of a
 *        (1, 1) error on its pair, lambda1 lambda2 / (lambda1 + lambda2).
 * @param lambda1 Band-1 wavelength (m).
 * @param lambda2 Band-2 wavelength (m).
 * @return d (m).
 */
double ringFixWindowReference(double lambda1, double lambda2);

/**
 * @brief The robust scale of the class's trailing-hour residuals of the links other than @p own.
 * @param buf     The class's (time, link index, residual) entries.
 * @param members The class's link index -> entries in @p buf.
 * @param own     The tested link's index (its entries are skipped).
 * @param scale   Written: 1.4826 times the MAD about the median (m), when formed.
 * @param n       Written: the other links behind it.
 * @return True when at least kRingFixWindowMinOthers other links have entries and the MAD is above zero.
 */
bool ringFixWindowScale(const std::deque<std::tuple<double, int, double>>& buf, const std::map<int, int>& members,
                        int own, double& scale, int& n);

/**
 * @brief One epoch of the monitor over this epoch's held links.
 * @param W       The monitor's state. Written: the links' statistics, the class buffers, the counters.
 * @param held    This epoch's held links (the links held before this epoch's decisions), in the links' order.
 * @param now_s   The epoch (continuous GPS seconds).
 * @param dt_s    The epoch interval (s): O1's coverage and the shares' overlap count rows of it.
 * @param rows    Written when not null: one row per held link.
 * @return The episodes' edges of this epoch: the closes first, then the opens, each in the links' order.
 */
std::vector<RingFixWindowEdge> ringFixWindowStep(RingFixWindowMonitor& W, const std::vector<RingFixWindowObs>& held,
                                                 double now_s, double dt_s, std::vector<RingFixWindowRow>* rows);

/**
 * @brief The monitor's seam in the decide epoch (after the audit, before the decisions): reads the held links and
 *        their residuals, runs ringFixWindowStep, writes network_ring_fix_window.csv and the RINGFIX window_open /
 *        window_close events.  Returns at once without network.ring_fix.window_monitor.
 * @param run      The run. Written: run.ctx.carry.ring_fix->window, run.traces. Read: the ring fix's links,
 *                 run.setup.stations.
 * @param ep       The epoch: ep.se.time, ep.clock.
 * @param readings This epoch's chord readings (the residuals).
 * @param audit    This epoch's audit (held_before: the links held before this epoch's decisions).
 */
void networkRingFixWindowMonitor(NetworkRun& run, const NetworkEpoch& ep, const RingFixDecideReadings& readings,
                                 const RingFixDecideAudit& audit);

}  // namespace GnssProcessor
