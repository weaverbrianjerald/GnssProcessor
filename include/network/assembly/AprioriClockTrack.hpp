#pragma once
/**
 * @file AprioriClockTrack.hpp
 * @brief The broadcast a priori clock tracked by the network's own phase
 *        (@c network.apriori_clock_track): each satellite's a priori follows the satellite-common
 *        between-epoch change that the IF-TD two-way centring measures, before the station rows
 *        are built.
 *
 * ## Problem addressed
 *
 * Under @c network.apriori_source @c sp3_brdc_clock the satellite clock state is a correction to
 * the continuous broadcast clock a(t) and walks at the anchor's q, so its prior mean at epoch t is
 * the previous posterior: the time update assumes the real clock moves as the broadcast polynomial
 * does.  It does not: the broadcast misfit increment (the real clock against the polynomial) has a
 * root mean square of 17.5 mm per 30 s on DOY 129 (p99 67 mm; G12's +0.09 m step at 04:18 is in
 * the CODE clock and not in the broadcast one).  Each epoch the solve corrects only part of that
 * prior bias, and the remainder accumulates in the phase-visible clock the smoothed hold passes to
 * a user: against CODE it changes by 4.6 / 8.5 / 13.7 / 22.6 mm over 1 / 10 / 40 / 120 epochs,
 * where the same estimator on the CODE a priori reads 2.0 / 3.9 / 5.2 / 6.7 mm (card BRD).
 *
 * ## The mechanism
 *
 * The IF-TD two-way centring (@c network.cycle_slip.if_phase_td_sat_centre, network/slip/SlipQc.hpp)
 * fits each epoch a column per satellite: the satellite's common between-epoch change of the
 * ionosphere-free phase against the a priori, over the stations that track it (median polish).  The
 * column is -(dm) to within about 3 mm per epoch, dm the misfit's increment.  This seam adds the
 * increment to a per-satellite track U (U += -column) and moves every modelled observation of the
 * satellite at every station by -U (a satellite clock larger by U lowers the modelled range by U),
 * after the PASS-A slip scans and before the station rows.  The solve then works against the a
 * priori a + U: its clock time update starts from a mean that follows the network's own phase, so
 * the anchor's walk carries only the column's error.  Nothing in the fold changes: the walk factor
 * c(t) - c(t-1) = 0 at the anchor's q now relates two states on frames U(t) and U(t-1), which is the
 * time update with the control input -column.
 *
 * The wire stays the estimate against the stamped broadcast record: the broadcast wire seam adds U
 * after the hold (C_wire = C_est + O + U, @ref GnssProcessor::networkBrdcWire "networkBrdcWire"), so a user reconstructs
 * brdc(iode) + C_wire = a + U + C_est, the estimated clock, and the smoothed hold, which holds
 * (C - P) to the smoothed motion of the solve's own state, passes the solve's motion on top of the
 * tracked a priori exactly as it passed it on top of a(t).
 *
 * U is a frame offset, never a statement: a satellite with no fitted column this epoch keeps its U
 * (the a priori moves with the broadcast increment, the behaviour without the knob), and its clock
 * state absorbs whatever U carries at a re-seed.  The column's slow error (it drifts about 90 mm per
 * hour when summed, the low-elevation samples' troposphere) is corrected each epoch by the data,
 * since the column only sets the prior mean.
 *
 * ## Both edges
 *
 * A satellite's first tracked epoch and every resumption after an epoch without a column are
 * counted and named (a @c BRD_TRACK event: @c track_start, @c track_resume with the epochs paused);
 * every epoch without a column for a tracked satellite is counted as paused.  Run-end line: the
 * counts, the |increment| ladder and the largest |U| (@ref GnssProcessor::networkBrdcWireRunEnd "networkBrdcWireRunEnd").
 *
 * Default off; requires @c sp3_brdc_clock and the two-way centring (refused at load otherwise);
 * fingerprinted (the carried clock is on the tracked frame), fresh convergence only.
 */

#include <array>
#include <map>
#include <string>
#include <vector>

namespace GnssProcessor {

struct NetworkRun;
struct NetworkEpoch;
struct EpochPassA;
struct PppObservation;

/// What one epoch of tracking did, for the counters and the events.
struct AprioriTrackEpoch {
    int applied{0};                         ///< Satellites whose track moved by a column this epoch.
    int paused{0};                          ///< Tracked satellites with no column this epoch.
    std::vector<std::string> started;       ///< Satellites tracked for the first time.
    std::map<std::string, int> resumed;     ///< Satellites resuming, with the epochs they were paused.
    std::array<long long, 6> inc_hist{};    ///< |increment| below 5, 10, 20, 50, 100 mm and above.
    double max_abs_track_m{0.0};            ///< Largest |U| after the update (m).
    long long rows_moved{0};                ///< Modelled observations moved by a nonzero U.
};

/**
 * @brief Advance the tracks by this epoch's columns and move the modelled observations.
 *
 * For every satellite with a column c: U += -c.  Every modelled code and phase value of a
 * satellite with a track at every station is lowered by its U (a Doppler entry, a range rate,
 * is left alone).  Pure: the caller
 * owns the tracks, the pause counts and the modelled observations.
 *
 * @param columns  Satellite -> this epoch's IF-TD column (m), fitted columns only.
 * @param track    Satellite -> U (m); updated in place.
 * @param paused   Satellite -> consecutive epochs without a column since its last one; updated.
 * @param modeled  Per station, the epoch's modelled observations; moved in place.
 * @return The epoch's record.
 */
AprioriTrackEpoch aprioriClockTrackApply(const std::map<std::string, double>& columns,
                                         std::map<std::string, double>& track,
                                         std::map<std::string, int>& paused,
                                         std::vector<std::vector<PppObservation>>& modeled);

/**
 * @brief The seam: one epoch of the tracked broadcast a priori (@c network.apriori_clock_track).
 *
 * Reads the IF-TD centring's columns of the epoch (@c ep.events.slip.sat_centre_col), advances
 * the satellites' tracks (the satellite table's @c apriori_track_m, @c apriori_track_paused),
 * moves the epoch's modelled observations, and writes the events and counters.  Runs after the
 * PASS-A slip scans (the columns are theirs) and before the pre-assembly filters and the station
 * rows (they read the moved values).  No-op unless the knob is armed.
 *
 * @param run    The run (counters, traces, the satellite table).
 * @param ep     The epoch (its time and the slip events).
 * @param pass_a The PASS-A buffers; @c modeled is written.
 */
void networkAprioriClockTrack(NetworkRun& run, const NetworkEpoch& ep, EpochPassA& pass_a);

} // namespace GnssProcessor
