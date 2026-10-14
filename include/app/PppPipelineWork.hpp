#pragma once

/**
 * @file PppPipelineWork.hpp
 * @brief The working values the stages of the PPP / PPP-RTK pipeline share.
 *
 * Application::runPpp (src/core/app/Application.cpp) is a short ordered driver over named
 * private stages (card UOM step U7b).  The values one epoch's stages hand to one another
 * were locals of the epoch loop of a single 836-line Application::run; they are now the
 * members of @ref PppEpochWork, built empty at the top of each epoch and discarded at its
 * end.  Each stage opens with an alias block that binds the names the code had as loop
 * locals (`const auto& current_time = w.current_time;`), so its body reads as it did;
 * the driver passes members to the helpers that take explicit arguments
 * (`handleGap(w.dt, w.long_gap, w.epoch_prefix, w.wls_result)`).
 *
 * @ref PppRunEndView is what the run-end reports (src/core/app/PppRunEnd.cpp) read beyond
 * the Application's own members.
 *
 * What persists across epochs is not here: it is an Application member (first_epoch_,
 * prev_tow_, prev_epoch_time_, last_processed_tow_, max_duration_anchor_s_, ppp_state_,
 * smoother_buffer_ and the components).
 *
 * Included by Application.cpp, the PppEpoch*.cpp and PppRunEnd.cpp stage files, and
 * tests/test_ppp_epoch_work.cpp.
 */

#include "common/EpochMeasurement.hpp"
#include "common/GnssTypes.hpp"
#include "common/PppObservation.hpp"
#include "math/TimeUtils.hpp"
#include "math/WlsEstimator.hpp"
#include "rinex/RinexParser.hpp"
#include "slip/CycleSlipManager.hpp"
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace GnssProcessor {

/**
 * @brief The values the stages of one PPP / PPP-RTK epoch share.
 *
 * Built empty by Application::runPpp at the top of each epoch, filled and read by the
 * stages in spine order, discarded when the epoch ends.  The members are grouped by the
 * stage that first writes them, and each default is the initializer the epoch loop's
 * local had, so an epoch starts from the same values it did before the cut.
 */
struct PppEpochWork {
    // --- 4a: the epoch's observations (Application::collectEpochObservations) ---
    GnssTime current_time{};                  ///< Epoch time (GPS scale), of the epoch's first record.
    std::vector<RinexObservation> epoch_obs;  ///< The epoch's RINEX observations of the active systems.
    // The [%E] field in the spdlog pattern (LogManager) carries the GPS
    // week, TOW and GPS-scale datetime on every log line, so an epoch
    // prefix in the message body would duplicate it.  The variable stays,
    // empty, so the "{}..." format strings keep their argument.
    const std::string epoch_prefix;

    // --- 4b: the time step (Application::stepEpochClock_) ---
    double dt{ 0.0 };            ///< Seconds since the previous processed epoch; the nominal interval on the first.
    bool gap_detected{ false };  ///< dt exceeds the data-gap tolerance (never on the first epoch).
    bool long_gap{ false };      ///< The gap also exceeds processing.long_gap_tolerance_s.

    // --- 4d: the models (Application::computeEpochModels_) ---
    std::vector<PppObservation> modeled_obs;         ///< Geometry-resolved observations of the epoch.
    std::map<std::string, DropReason> drop_reasons;  ///< Why each satellite missing from modeled_obs was dropped.

    // --- 4e: the WLS solution (Application::handleWlsSeeding) ---
    WlsResult wls_result;  ///< Empty when the state has no receiver clock (BSSD); read by the gap policy.

    // --- 4g: the cycle slips (Application::detectCycleSlips) ---
    CycleSlipEpochResult slip_result;  ///< The slip phase's counters, reset satellites and integer repairs.

    // --- 4h: the measurement update (runFilterUpdateLoop, runGeometryRelinearization,
    //     mergeEpochResets_) ---
    EpochMeasurement applied_meas{};         ///< The accepted measurement set, if any.
    bool have_applied{ false };              ///< At least one update was applied this epoch.
    std::set<std::string> epoch_reset_sats;  ///< Satellites reset this epoch: update loop, relinearization, slip phase.
    bool epoch_complete_reset{ false };      ///< The filter performed a complete reset this epoch.
};

/**
 * @brief What the PPP run-end reports read beyond the Application's own members.
 *
 * Built once by Application::runPpp when the epoch loop has ended and handed to every row
 * of the run-end table (src/core/app/PppRunEnd.cpp).
 */
struct PppRunEndView {
    std::chrono::high_resolution_clock::time_point wall_start;  ///< Wall clock when the epoch loop started.
    bool is_ppp_rtk;  ///< The processing mode is PPP-RTK (the timing and re-inflation lines name the mode).
};

} // namespace GnssProcessor
