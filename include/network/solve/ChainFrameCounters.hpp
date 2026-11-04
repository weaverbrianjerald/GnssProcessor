#pragma once
/**
 * @file ChainFrameCounters.hpp
 * @brief The run tallies of the eq34 chain-frame observatory (network/solve/ChainFrame.hpp), a
 *        base of NetworkServiceCounters.
 *
 * Kept apart from ChainFrame.hpp so that the run context, which every network translation unit
 * includes, carries only these tallies: the observatory's memory of the last observed epoch is
 * held through a pointer to a type the run context never needs complete.
 */

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <string>

namespace GnssProcessor {

struct ChainFrameMemory;   // network/solve/ChainFrame.hpp

/// The number of rows of the chain-event table (ChainEventRow in network/solve/ChainFrame.hpp).
inline constexpr std::size_t kChainEventRowCount = 9;

/// Run-scope counters and memory of the frame observatory, a base of NetworkServiceCounters
/// (network/state/RunContext.hpp).  All zero, and no memory, unless network.trace_chain_datum
/// is armed.
struct ChainFrameCounters {
    std::shared_ptr<ChainFrameMemory> chain_frame_memory;   ///< The last observed epoch (null before the first).
    long long cf_epochs{0};                        ///< Epochs observed.
    long long cf_edges{0};                         ///< Tree edges recorded.
    std::array<long long, kChainEventRowCount> cf_rows{};   ///< Edges by table row.
    std::array<long long, kChainEventRowCount> cf_rows_armed{};    ///< ...decided by the row's integer rule (@c network.phase_datum_eq34_event_rows).
    std::array<long long, kChainEventRowCount> cf_rows_frac{};     ///< ...whose step as realized was a known nonzero step that is not an integer of cycles (the as-built step on a row that is not armed).
    std::array<long long, kChainEventRowCount> cf_rows_unknown{};  ///< ...whose step as realized was unknown (a re-reference; the as-built step on a row that is not armed).
    // The kept rows written to the event trace.  A row marks the election at which the as-built rule would
    // have moved the edge (a reversed arc, an unknown step, or a change of the as-built target), so a
    // difference between the as-built target and the frozen one that persists is counted once.
    long long cf_kept_reversed{0};                 ///< Reversed arcs the kept row held, each written as a leave row and a join row.
    long long cf_kept_hold_whole{0};               ///< Hold rows whose as-built step was a nonzero whole number of cycles (within 1e-6).
    long long cf_kept_hold_other{0};               ///< Hold rows whose as-built step was nonzero and not whole cycles.
    long long cf_kept_hold_unknown{0};             ///< Hold rows whose as-built step was unknown.
    // The returning rows written to the event trace.  An edge the returning row released writes a leave row and a
    // join row; the as-built step is the one the election drafted before the release replaced it.
    long long cf_ret_pairs{0};                     ///< Released edges written as a leave row and a join row.
    long long cf_ret_asbuilt_zero{0};              ///< ...whose as-built step was a known zero.
    long long cf_ret_asbuilt_whole{0};             ///< ...a known nonzero whole number of cycles (within 1e-6).
    long long cf_ret_asbuilt_other{0};             ///< ...a known nonzero step that is not whole cycles.
    long long cf_ret_asbuilt_unknown{0};           ///< ...an unknown step.
    // The rehost rows written to the event trace.  An edge the rehost row released keeps its leave row and join
    // row and appends the as-built facts the release replaced.
    long long cf_rh_pairs{0};                      ///< Released re-host edges written as a leave row and a join row.
    long long cf_rh_asbuilt_zero{0};               ///< ...whose as-built step was a known zero.
    long long cf_rh_asbuilt_whole{0};              ///< ...a known nonzero whole number of cycles (within 1e-6).
    long long cf_rh_asbuilt_other{0};              ///< ...a known nonzero step that is not whole cycles.
    long long cf_rh_asbuilt_unknown{0};            ///< ...an unknown step.
    long long cf_rh_certified_held{0};             ///< ...released edges the previous chord table certified (the certified branch is held, so they were released anyway).
    long long cf_ret_cut_releases{0};              ///< Cut releases of the returning row (one cut_release row each).
    long long cf_ret_declared_keys{0};             ///< Keys the returning row's releases declared (one declared_keys row each).
    long long cf_rh_cut_releases{0};               ///< Cut releases of the rehost row (one cut_release row each).
    long long cf_rh_declared_keys{0};              ///< Keys the rehost row's releases declared (one declared_keys row each).
    long long cf_cut_releases{0};                  ///< Cut releases of the returning and rehost rows (one cut_release row each).
    long long cf_nonint_targets{0};                ///< Pins whose target is not an integer of cycles.
    long long cf_soft_pins{0};                     ///< Soft pins (sigma multiplier above 1).
    long long cf_bridges{0};                       ///< Shared-side bridge pins.
    long long cf_d4_keys{0};                       ///< (sat, signal) keys the legacy rule re-referenced (D4 erase).
    long long cf_known_shifts{0};                  ///< (sat, signal) keys the legacy shift walk moved.
    long long cf_frac_shifts{0};                   ///< ...by a step that is not an integer of cycles.
    long long cf_rehosts{0};                       ///< Re-hosts classified.
    std::array<long long, 3> cf_cert{};            ///< Re-hosts by certification source (none, pair, lattice).
    long long cf_cert_missing{0};                  ///< Uncertified re-hosts whose new arc has no row in the previous chord table.
    long long cf_conflicts{0};                     ///< Re-hosts whose pair and lattice integers disagree.
    long long cf_wrong_cand{0};                    ///< Certified re-hosts whose integer is not the nearest integer of the band's own float.
    long long cf_cut_arcs{0};                      ///< Arcs cut-released (0 unless a releasing row is armed).
    long long cf_cut_pb{0};                        ///< Phase-bias priors cut-released (0 unless a releasing row is armed).
    long long cf_cut_rpb{0};                       ///< Receiver phase biases cut-released (0 unless a releasing row is armed).
    long long cf_declared_keys{0};                 ///< Keys declared moved (0 unless a releasing row is armed).
    long long cf_root_handovers{0};                ///< Components whose datum receiver changed.
    long long cf_merges{0};                        ///< Components of the last epoch absorbed into another.
    long long cf_splits{0};                        ///< Components split off a component of the last epoch.
    long long cf_returning{0};                     ///< Returning vertices.
    long long cf_flipped{0};                       ///< Kept edges whose direction flipped.
    long long cf_departed{0};                      ///< Vertices of the last epoch absent from this one.
    long long cf_gated_keys{0};                    ///< Usable keys gated out of the union.
    std::map<std::string, int>       cf_comp_max;  ///< group -> the most components in one epoch.
    std::map<std::string, long long> cf_comp_sum;  ///< group -> components summed over the epochs.
    std::map<std::string, long long> cf_comp_epochs;   ///< group -> epochs observed.
    long long cf_pin_epochs{0};                    ///< Epochs whose pin identity was checked.
    long long cf_pin_mismatch_epochs{0};           ///< ...where declared, traced and folded pins disagreed.
    long long cf_chord_rows{0};                    ///< Chord table rows written.
    long long cf_chord_pair_det{0};                ///< ...with a band determined by the two-band fix.
    long long cf_chord_lattice{0};                 ///< ...with a band the lattice implies.
    long long cf_chord_conflicts{0};               ///< ...where the two disagree.
    long long cf_events{0};                        ///< CHAIN rows written.
    long long cf_rpb_rows{0};                      ///< network_rpb_states.csv rows written.
    double    cf_rpb_nees_sum{0.0};                ///< Sum of the time-link NEES terms.
    long long cf_rpb_nees_n{0};                    ///< Their count.
    long long cf_pb_prehold_rows{0};               ///< network_pb_prehold.csv rows written.
};

} // namespace GnssProcessor
