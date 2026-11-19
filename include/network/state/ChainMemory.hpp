#pragma once
/**
 * @file ChainMemory.hpp
 * @brief The chain's and the ring fix's memory across a resume: the snapshot's chain memory section.
 *
 * A run resumed from a snapshot otherwise starts the chain's tree memory and the whole ring fix
 * empty: every held link and its integers, the settled flags' waits, the untied satellites'
 * moves, the vertices seen in earlier trees and the pinned lives of the tree edges.  With
 * @c network.state_snapshot_chain_capture the snapshot carries that memory as one section of its
 * own, from its header line @c CHAIN,v2,<fingerprint> through @c CHEND, in four parts, each written
 * and read by its owner:
 *  - the broadcast group-delay latch and the window it was latched over (the setup's
 *    @c sat_gf_target_m), lines @c GW and @c GD, here;
 *  - the chain's memories in the entity tables (the vertices seen in earlier trees, the last
 *    election's tree roots, the root row's pinned receiver phase-bias targets, the pinned lives
 *    of the tree edges, the arcs' anchor ages, the leveling carries' re-level stamps, the
 *    satellites' code-reference events, the emitter's anchor baseline) and the topology ledger's
 *    memory (network.trace_topology), lines @c C*, @ref net::chainMemoryWriteEntities and here;
 *  - the ring fix's memory and its two hand-offs, lines @c R*, @ref ringFixWriteMemory;
 *  - the emitter's state that depends on the held values (the step declaration's baselines,
 *    the unsettled steps' last flags, the continuity guard), lines @c E*,
 *    @c NetworkProductEmitter::writeHeldValueState.
 * The header's fingerprint covers the keys that give the lines their meaning
 * (@ref networkChainMemoryFingerprintTokens); the snapshot's own configuration fingerprint does not
 * carry them, so neither the capture nor the restore changes whether a warm resume is admitted.
 *
 * With @c network.resume_chain_restore a warm resume stages the section and
 * @ref networkChainMemoryRestore puts each part back before the first epoch, when the section's
 * fingerprint matches the run's; the leg then uses the saved latch in place of latching over its
 * own window, and its first election judges each restored pinned life on the leg's own ledger and
 * flags.  A section that is absent or refused restores nothing (counted and logged) and the warm
 * resume goes on.  Run counters are not saved (each leg reports its own), nor is the near-null
 * monitor's measure-only state.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace GnssProcessor {

class GnssConfig;
struct NetworkRun;

/**
 * @brief The tokens of the chain memory section's fingerprint: the configuration keys that give
 *        its lines their meaning.
 *
 * The ring fix's rules, pairs, gate, flags, guards and cascade (the held links, their integers and
 * the settled flags), the chain's family, family pairs and event rows (the tree memory), and the
 * step-class pairs, whose order indexes the emitter's step baselines.
 *
 * @param config The configuration.
 * @return One token per key or list entry, in a fixed order.
 */
std::vector<std::string> networkChainMemoryFingerprintTokens(const GnssConfig& config);

/**
 * @brief The chain memory section's fingerprint: the FNV-1a hash of its tokens.
 * @param config The configuration.
 * @return The fingerprint the section's header carries, and its restore compares.
 */
std::uint64_t networkChainMemoryFingerprint(const GnssConfig& config);

/**
 * @brief Write the chain memory section of the snapshot (network.state_snapshot_chain_capture).
 *
 * Called by the snapshot writer after the epoch's emit; reads the run and changes nothing.
 *
 * @param run The run: its configuration (the header's fingerprint), the setup's latch, the entity
 *            tables, the ring fix and the emitter.
 * @return The section, newline-terminated lines from its @c CHAIN header through @c CHEND.
 */
std::string networkChainMemoryWrite(const NetworkRun& run);

/**
 * @brief Restore the chain memory section a warm resume staged (network.resume_chain_restore).
 *
 * Called by the driver once, after the warm resume and before the first epoch; a no-op when
 * nothing was staged.  When the section's header carries this build's version and a fingerprint
 * equal to the run's (@ref networkChainMemoryFingerprint), it puts back the latch (in place of the
 * leg's own), the chain's memories, the ring fix's memory and the emitter's held-value state, hands
 * the restored pinned lives to the first election, and logs one line with what it restored.
 * Otherwise it restores nothing, and a warning and a counter say so; the warm resume goes on.  The
 * staged text is consumed.
 *
 * @param[in,out] run The run; writes the setup's latch, the entity tables, the ring fix, the
 *                    emitter and the restore's counters.
 */
void networkChainMemoryRestore(NetworkRun& run);

}  // namespace GnssProcessor
