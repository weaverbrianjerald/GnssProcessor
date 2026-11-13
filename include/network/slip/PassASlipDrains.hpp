#pragma once
/**
 * @file PassASlipDrains.hpp
 * @brief The serial response drains of the PASS A slip scan as named stages: the MW refloat
 *        with the datum re-reference queues, the IF-phase driver, the arc-scoped GF jump,
 *        the wind-up restart, the gap re-entry pair and the injected free_epoch response.
 *
 * Each stage wraps one drain seam of SlipQc.hpp or GfArcJump.hpp with the explanation of
 * why it sits at its place in the epoch. The bridge pends, the re-level state pointer and
 * the common-band pointer are the values the drains share; they are explicit parameters.
 * Called in order by `Application::networkPassASlipScan_`.
 */

#include "network/slip/PassASlipScan.hpp"

namespace GnssProcessor {

/**
 * @brief MW-detect / LLI refloat drain with the datum re-reference queues and the bridge
 *        pends it works on. Returns the epoch's bridge pends, which the later drains keep
 *        filling and the driver writes back.
 *
 * @param v               The PASS A view of the epoch.  The drain writes through it to the stations
 *                        (pending-reseed marks), the slipped-ambiguity set and the slip-QC channel.
 * @param mw_carry_warm   True when the drain may refloat the carry; false (a cold carry, or the
 *                        first epochs after a warm resume) makes it diagnostic-only.
 * @param datum_detect_on True when the datum-station slip detect is armed: the drain then also takes
 *                        the datum re-reference queues (copied in from the satellite table and
 *                        written back after the drain), the reference-satellite mark set and the
 *                        leveling carry.
 * @param relevel_ptr     The epoch's free-epoch latch, or nullptr when
 *                        @c network.cycle_slip.relevel_free_epoch is off.
 * @param common_band_ptr The epoch's common-band scope verdicts, or nullptr when the scope is not
 *                        armed (every signal may then be freed).
 * @return The epoch's bridge pends, keyed by (station index, satellite, signal pair), copied in from
 *         the lines of sight and updated by this drain; empty when none are outstanding.  The later
 *         stages take it by reference.
 */
std::map<net::ArcTuple, RelevelPend> passAMwRefloatStage(const PassAView& v, bool mw_carry_warm,
                                                         const bool datum_detect_on,
                                                         RelevelEpochState* relevel_ptr,
                                                         const CommonBandEpoch* common_band_ptr);

/**
 * @brief IF-phase triple-difference slip driver.
 *
 * @param v                The PASS A view of the epoch (stations, presence, slip-QC channel and
 *                         flag-rate counter are written through it).
 * @param relevel_ptr      The epoch's free-epoch latch, or nullptr when
 *                         @c network.cycle_slip.relevel_free_epoch is off.
 * @param relevel_pend     The epoch's bridge pends, updated in place: a flagged pair creates or
 *                         refreshes its pend and a fresh free voids a stale one.  Left untouched
 *                         when @p relevel_ptr is null.
 * @param common_band_ptr  The epoch's common-band scope verdicts, or nullptr when the scope is not
 *                         armed (the pair refloat then frees both bands).
 * @param sat_clock_ptr The satellite clock event sink (the epoch's
 *                      @c EpochEvents::Slip::sat_clock_step and the run's slip-QC
 *                      counters), non-null only when
 *                      @c network.cycle_slip.if_phase_td_sat_clock_event is armed.
 * @param sat_centre_ptr The two-way centring sink (its minimum column size and the run's
 *                      slip-QC counters), non-null only when
 *                      @c network.cycle_slip.if_phase_td_sat_centre is armed.
 */
void passAIfTdStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                    std::map<net::ArcTuple, RelevelPend>& relevel_pend,
                    const CommonBandEpoch* common_band_ptr,
                    IfTdSatClockSink* sat_clock_ptr,
                    IfTdSatCentreSink* sat_centre_ptr = nullptr);

/**
 * @brief Arc-scoped GF jump trigger, then the AR search's suspect-corner stamps for every
 *        GF flag.
 *
 * @param v               The PASS A view of the epoch (stations, presence and slip-QC channel are
 *                        written through it).
 * @param relevel_ptr     The epoch's free-epoch latch, or nullptr when
 *                        @c network.cycle_slip.relevel_free_epoch is off.
 * @param relevel_pend    The epoch's bridge pends, updated in place (a fresh free voids a stale
 *                        pend); left untouched when @p relevel_ptr is null.
 * @param common_band_ptr The epoch's common-band scope verdicts, or nullptr when the scope is not
 *                        armed.
 */
void passAGfArcJumpStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                         std::map<net::ArcTuple, RelevelPend>& relevel_pend,
                         const CommonBandEpoch* common_band_ptr);

/**
 * @brief Wind-up accumulator-restart response: refloats the arcs whose accumulator
 *        restarted this epoch.
 *
 * @param v            The PASS A view of the epoch (stations, presence, slip-QC counters and the
 *                     leveling carry are read or written through it).
 * @param relevel_ptr  The epoch's free-epoch latch, or nullptr when
 *                     @c network.cycle_slip.relevel_free_epoch is off.
 * @param relevel_pend The epoch's bridge pends; a restart is a model step, so the pends of a
 *                     restarted (station, satellite) are voided.
 */
void passAWindupRestartStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                             std::map<net::ArcTuple, RelevelPend>& relevel_pend);

/**
 * @brief Gap re-entry refloat, row-level then per signal, over the arc ledger's
 *        re-entry population when the ledger is armed.
 *
 * @param v            The PASS A view of the epoch (stations, presence, slip-QC channel, the
 *                     leveling carry and the epoch's slip events are used through it).
 * @param relevel_ptr  The epoch's free-epoch latch, or nullptr when
 *                     @c network.cycle_slip.relevel_free_epoch is off.
 * @param relevel_pend The epoch's bridge pends, updated in place (a fresh mark voids a pend on the
 *                     same station and satellite); left untouched when @p relevel_ptr is null.
 */
void passAGapReentryStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                          std::map<net::ArcTuple, RelevelPend>& relevel_pend);

/**
 * @brief Injected free_epoch arc response of the event harness. Unreachable unless the
 *        injector staged a free_epoch directive.
 *
 * @param v            The PASS A view of the epoch (stations, presence and the slipped-ambiguity
 *                     set are used through it).
 * @param relevel_ptr  The epoch's free-epoch latch, or nullptr when
 *                     @c network.cycle_slip.relevel_free_epoch is off (the mark alone cannot then
 *                     deliver the free epoch).
 * @param relevel_pend The epoch's bridge pends, updated in place; left untouched when
 *                     @p relevel_ptr is null.
 */
void passAInjectedFreeStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                            std::map<net::ArcTuple, RelevelPend>& relevel_pend);

} // namespace GnssProcessor
