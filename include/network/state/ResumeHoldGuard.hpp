#pragma once
/**
 * @file ResumeHoldGuard.hpp
 * @brief The resume's hold guard: restored ring-fix holds are released when the wind-up accumulators did not come back.
 *
 * A warm resume under @c network.resume_chain_restore puts back the ring fix's links, held integers included, in the
 * writer's frame.  Under accumulated wind-up (estimation.accumulate_phase_windup) the writer's integers absorb each
 * arc's whole wind-up turns since the arc began.  When the station memory section (every station's wind-up
 * accumulator, @c network.resume_station_memory_restore) is not put back, every accumulator starts again at the raw
 * angle of the resumed run's first epoch, and each restored hold on an arc whose accumulator had passed half a cycle
 * sits one whole turn off: a hold the phase contradicts (card WRS, 2026-11-23: 4,692 link-epochs on 134 links of the
 * simulator leg `evleg_wr_s_L`, one cycle on both bands).
 *
 * Two edges guard it.  The loader refuses the chain restore under accumulated wind-up without the station memory
 * restore.  At run time the station memory section can still be absent (a writer without its capture) or refused
 * (its fingerprint); then @ref networkResumeHoldGuardRelease releases every restored ring-fix hold before the first
 * epoch, counted, and @ref networkResumeHoldGuardSeamEvent declares the release at the first resumed epoch.  The links
 * re-enter through the ring fix's ordinary gate, after its own window from the release (the snapshot's epoch).
 */

namespace GnssProcessor {

struct NetworkEpoch;
struct NetworkRun;

/**
 * @brief Release every restored ring-fix hold when the chain section was restored and the station memory was not.
 *
 * Acts only when the run keeps a ring fix, the wind-up is accumulated, the chain memory restore restored its section
 * and the station memory restore did not (its section absent or refused).  Each held link is un-held as a release at
 * the snapshot's epoch; the count goes to the run's counters and one line to the log.  A no-op otherwise.  Must run
 * after both restores and before the first epoch.
 *
 * @param[in,out] run The run: its ring fix's links and the guard's counters.
 */
void networkResumeHoldGuardRelease(NetworkRun& run);

/**
 * @brief The seam event of the hold guard: one row at the first resumed epoch (a measure-only observer,
 *        @c kNetworkEpochObservers, slot Admitted).
 *
 * The row (tier @c RESUME, event @c hold_guard_release) gives the number of restored holds released, by
 * constellation, and why (the station memory section absent or refused).  Written once, and only when the guard
 * released.
 *
 * @param run The run: the guard's counters and the events trace.
 * @param ep  The epoch (its time stamps the row).
 */
void networkResumeHoldGuardSeamEvent(NetworkRun& run, const NetworkEpoch& ep);

}  // namespace GnssProcessor
