#pragma once
/**
 * @file ChainMemory.hpp
 * @brief The chain's and the ring fix's memory across a resume: the snapshot's chain memory section.
 *
 * A run resumed from a snapshot otherwise starts the chain's tree memory and the whole ring fix
 * empty: every held link and its integers, the settled flags' waits, the untied satellites'
 * moves, the vertices seen in earlier trees and the pinned lives of the tree edges.  With
 * @c network.state_snapshot_chain_capture the snapshot carries that memory as one section of its
 * own (@c CHAIN header through @c CHEND), in four parts, each written and read by its owner:
 *  - the broadcast group-delay latch and the window it was latched over (the setup's
 *    @c sat_gf_target_m), lines @c GW and @c GD, here;
 *  - the chain's memories in the entity tables (the vertices seen in earlier trees, the last
 *    election's tree roots, the root row's pinned receiver phase-bias targets, the pinned lives
 *    of the tree edges, the arcs' anchor ages, the emitter's anchor baseline) and the topology ledger's memory
 *    (network.trace_topology), lines @c C*, @ref net::chainMemoryWriteEntities and here;
 *  - the ring fix's memory and its two hand-offs, lines @c R*, @ref ringFixWriteMemory;
 *  - the emitter's state that depends on the held values (the step declaration's baselines,
 *    the unsettled steps' last flags, the continuity guard), lines @c E*,
 *    @c NetworkProductEmitter::writeHeldValueState.
 * With @c network.resume_chain_restore a warm resume stages the section and
 * @ref networkChainMemoryRestore puts each part back before the first epoch; the leg then uses
 * the saved latch in place of latching over its own window.  Run counters are not saved (each
 * leg reports its own), nor is the near-null monitor's measure-only state.
 */

#include <string>

namespace GnssProcessor {

struct NetworkRun;

/**
 * @brief Write the chain memory section of the snapshot (network.state_snapshot_chain_capture).
 *
 * Called by the snapshot writer after the epoch's emit; reads the run and changes nothing.
 *
 * @param run The run: the setup's latch, the entity tables, the ring fix and the emitter.
 * @return The section, newline-terminated lines from its @c CHAIN header through @c CHEND.
 */
std::string networkChainMemoryWrite(const NetworkRun& run);

/**
 * @brief Restore the chain memory section a warm resume staged (network.resume_chain_restore).
 *
 * Called by the driver once, after the warm resume and before the first epoch.  Puts back the
 * latch (in place of the leg's own), the chain's memories, the ring fix's memory and the
 * emitter's held-value state, and logs one line with what it restored.  A no-op when nothing
 * was staged; the staged text is consumed.
 *
 * @param[in,out] run The run; writes the setup's latch, the entity tables, the ring fix and the
 *                    emitter.
 */
void networkChainMemoryRestore(NetworkRun& run);

}  // namespace GnssProcessor
