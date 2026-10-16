#pragma once
/**
 * @file CongruenceSetup.hpp
 * @brief Setup builder of the station congruence census (card NOM3 step EP4).
 *
 * The builder is called once from Application::runNetwork (src/core/network/driver/ApplicationNetwork.cpp).
 */

namespace GnssProcessor {

struct NetworkRun;

/**
 * @brief Seats the station congruence census when it is armed.
 *
 * A census arm.  An unarmed run keeps the default-constructed unarmed instance.  Setup builder
 * (card NOM3 step EP4): called once from Application::runNetwork, in the order
 * scripts/nom3_designs/EP4_design.md section 2 states.
 *
 * @param run The network run; fills run.ctx.carry.cg.
 */
void networkCongruenceArm(NetworkRun& run);

} // namespace GnssProcessor
