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
 */
std::map<net::ArcTuple, RelevelPend> passAMwRefloatStage(const PassAView& v, bool mw_carry_warm,
                                                         const bool datum_detect_on,
                                                         RelevelEpochState* relevel_ptr,
                                                         const CommonBandEpoch* common_band_ptr);

/**
 * @brief IF-phase triple-difference slip driver.
 */
void passAIfTdStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                    std::map<net::ArcTuple, RelevelPend>& relevel_pend,
                    const CommonBandEpoch* common_band_ptr);

/**
 * @brief Arc-scoped GF jump trigger, then the AR search's suspect-corner stamps for every
 *        GF flag.
 */
void passAGfArcJumpStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                         std::map<net::ArcTuple, RelevelPend>& relevel_pend,
                         const CommonBandEpoch* common_band_ptr);

/**
 * @brief Wind-up accumulator-restart response: refloats the arcs whose accumulator
 *        restarted this epoch.
 */
void passAWindupRestartStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                             std::map<net::ArcTuple, RelevelPend>& relevel_pend);

/**
 * @brief Gap re-entry refloat, row-level then per signal, over the arc ledger's
 *        re-entry population when the ledger is armed.
 */
void passAGapReentryStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                          std::map<net::ArcTuple, RelevelPend>& relevel_pend);

/**
 * @brief Injected free_epoch arc response of the event harness. Unreachable unless the
 *        injector staged a free_epoch directive.
 */
void passAInjectedFreeStage(const PassAView& v, RelevelEpochState* relevel_ptr,
                            std::map<net::ArcTuple, RelevelPend>& relevel_pend);

} // namespace GnssProcessor
