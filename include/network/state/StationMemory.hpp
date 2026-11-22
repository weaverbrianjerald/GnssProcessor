#pragma once
/**
 * @file StationMemory.hpp
 * @brief The stations' own memory across a resume: the snapshot's station memory section.
 *
 * A run resumed from a snapshot otherwise restarts each station's accumulated phase wind-up at
 * the raw angle of its first epoch, while the uninterrupted run continues its unwrapped angle.
 * The two then differ by whole cycles on every arc whose accumulator passed half a cycle since it
 * began, alike on every signal of the satellite, and the carries and held integers the resume
 * restores in the writer's frame sit a whole turn off in the resumed run's frame.  With
 * @c network.state_snapshot_station_memory_capture the snapshot carries that memory as one
 * section of its own, from its header line @c STMEM,v4,<fingerprint> through @c SMEND:
 *  - the wind-up accumulator of every satellite each station has seen, one line
 *    @c WU,<station>,<satellite>,<angle_rad>,<epoch_s>,<start_epoch_s> per entry, in station
 *    order and then satellite order.  The angle is in radians (never cycles: the conversion does
 *    not round-trip bit for bit), the times in continuous GPS seconds, every double in the
 *    shortest form that reads back to the same value;
 *  - the slip channel's processed-epoch counter, one line @c SQE,<epoch_idx>, so the epoch stamps of the
 *    arcs below keep their meaning (the resumed run's counter continues instead of restarting at 0);
 *  - the GF arc-jump trigger's arcs (@c network.cycle_slip.gf_arc_jump), one line
 *    @c GA,<station>,<satellite>,<last_epoch>,<last_t_s>,<sig1>,<sig2>,<gf_m>,<has_ref>,<ref_step_m>,
 *    <ref_span_s>,<flag_run>,<n>,<r_1>,...,<r_n> per arc: without them every arc restarts at the seam and
 *    flags without its robust scale for about 20 epochs (461 flags against 97 in the first 30 minutes of a
 *    resumed DOY 129 leg);
 *  - the raw slip history of every station: one line @c RH (the step scales and the LLI credibility),
 *    one line @c RP per satellite (its previous raw row and the epoch it came from) and one line @c RS per
 *    satellite and signal (the per-signal continuity record): without them a satellite whose gap spans the
 *    seam re-enters unmeasured and is refloated;
 *  - the gross line-of-sight screen's states: one line @c GL per line of sight (its streaks, its declaration
 *    and the cold-bound flag) and one line @c GT per station (the station tier): without them a line of
 *    sight or station declared before the seam folds at full weight in the resumed run until the evidence
 *    is re-earned.
 * The header's fingerprint covers the keys that give the angles and the arcs their meaning
 * (@ref networkStationMemoryFingerprintTokens); the snapshot's own configuration fingerprint
 * does not carry them, so neither the capture nor the restore changes whether a warm resume is
 * admitted.
 *
 * With @c network.resume_station_memory_restore a warm resume stages the section and
 * @ref networkStationMemoryRestore puts each entry back, verbatim, before the first epoch, when
 * the section's fingerprint matches the run's.  Stale entries (a satellite that set before the
 * snapshot) are restored too: the uninterrupted run keeps them, so a satellite returning past the
 * continuity horizon takes the restart branch and enters the restart ledger
 * (@c network.cycle_slip.windup_restart_refloat drains it) in both runs alike, where a resumed run
 * without the entry would seed silently.  A section that is absent or refused restores nothing
 * (counted and logged) and the warm resume goes on; a line for a station not in the run, or a
 * malformed line, is refused and counted, and so is every line of a station whose model keeps no
 * accumulator (fractional wind-up).
 */

#include <cstdint>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
struct NetworkRun;

/**
 * @brief The tokens of the station memory section's fingerprint: the configuration keys that
 *        give the saved angles their meaning.
 *
 * The wind-up model on or off, the accumulation posture, the continuity horizon, the
 * Sun-referenced frame, the noon-turn attitude and the gap bridge with its range, step and the
 * eclipse shadow scale it reads.
 *
 * @param config The configuration.
 * @return One token per key, in a fixed order.
 */
std::vector<std::string> networkStationMemoryFingerprintTokens(const GnssConfig& config);

/**
 * @brief The station memory section's fingerprint: the FNV-1a hash of its tokens.
 * @param config The configuration.
 * @return The fingerprint the section's header carries, and its restore compares.
 */
std::uint64_t networkStationMemoryFingerprint(const GnssConfig& config);

/**
 * @brief Write the station memory section of the snapshot
 *        (network.state_snapshot_station_memory_capture).
 *
 * Called by the snapshot writer after the epoch; reads every station's model and changes nothing.
 *
 * @param run The run: its configuration (the header's fingerprint) and the stations.
 * @param[out] n_records The number of record lines written (for the writer's counters).
 * @return The section, newline-terminated lines from its @c STMEM header through @c SMEND.
 */
std::string networkStationMemoryWrite(const NetworkRun& run, long long& n_records);

/**
 * @brief Restore the station memory section a warm resume staged
 *        (network.resume_station_memory_restore).
 *
 * A no-op when nothing was staged.  When the section's header carries this build's version and a
 * fingerprint equal to the run's (@ref networkStationMemoryFingerprint), it puts every record
 * back into its station's model and logs one line with the counts.  Otherwise it restores
 * nothing, and a warning and a counter say so; the warm resume goes on.  The staged text is
 * consumed.
 *
 * @param[in,out] run The run; writes the stations' wind-up accumulators and the restore's
 *                    counters.
 */
void networkStationMemoryRestore(NetworkRun& run);

}  // namespace GnssProcessor
