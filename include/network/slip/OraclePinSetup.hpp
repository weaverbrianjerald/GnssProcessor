#pragma once
/**
 * @file OraclePinSetup.hpp
 * @brief Setup builder of the oracle ambiguity pin (card NOM3 step EP4).
 *
 * The builder is called once from Application::runNetwork (src/core/network/driver/ApplicationNetwork.cpp).
 */

namespace GnssProcessor {

struct NetworkRun;
class LogManager;

/**
 * @brief Loads the oracle ambiguity pin (simulation only) when an `oracle_pin` inject event is armed.
 *
 * Throws on a missing or mis-pointed fixture, before any trace file opens.  Setup builder (card
 * NOM3 step EP4): called once from Application::runNetwork, in the order
 * scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.ctx.carry.oracle_pin.
 * @param log_manager_ The application's logging manager (Application::log_manager_).
 */
void networkOraclePinArm(NetworkRun& run, LogManager& log_manager_);

} // namespace GnssProcessor
