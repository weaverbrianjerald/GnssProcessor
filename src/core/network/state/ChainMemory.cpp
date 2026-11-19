/**
 * @file ChainMemory.cpp
 * @brief The chain memory section of the snapshot: its fingerprint, its writer and its restore
 *        (network.state_snapshot_chain_capture, network.resume_chain_restore).  The format and the parts are described
 *        in the header.
 */
#include "network/state/ChainMemory.hpp"

#include "config/GnssConfig.hpp"
#include "network/ar/RingFix.hpp"
#include "network/driver/Run.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkProductEmitter.hpp"
#include "products/NetworkStateSnapshot.hpp"   // NetworkStateSnapshotIO::fnv1a64

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace GnssProcessor {

namespace {

/// The section's format version, the second field of its header line.
constexpr const char* kChainMemoryVersion = "v2";

/// The fields of one comma-separated line (empty fields kept).
std::vector<std::string> splitFields(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    for (const char c : line) {
        if (c == ',') {
            f.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    f.push_back(cur);
    return f;
}

/// A continuous GPS time as its time of week (s), for the log.
double towOf(double gps_s) { return std::fmod(gps_s, 604800.0); }

/// The section fingerprint as the header writes it: 16 lower-case hexadecimal digits.
std::string fingerprintText(std::uint64_t fp) { return fmt::format("{:016x}", fp); }

}  // namespace

std::vector<std::string> networkChainMemoryFingerprintTokens(const GnssConfig& config) {
    const NetworkConfig& nc = config.getNetworkConfig();
    const auto& rf = nc.ring_fix;
    std::vector<std::string> t;
    // The ring fix's rules: they give the held links, their integers and the settled flags their meaning.
    t.push_back(fmt::format("rf={},{},{},{},{},{},{}", rf.enable, rf.rule, rf.held_first, rf.order, rf.gate,
                            rf.oracle_until_s, rf.held_first_by_life));
    for (const std::string& p : rf.pairs) t.push_back("rf_pair=" + p);
    t.push_back(fmt::format("rf_gate={},{},{},{},{}", rf.wl_frac_max, rf.sigma_scale, rf.code_event_window_s,
                            rf.max_decisions_per_epoch, rf.form_configured_pairs));
    t.push_back(fmt::format("rf_flags={},{},{},{},{},{},{},{}", rf.rehost_keep_carry, rf.settled_flags,
                            rf.settled_rule, rf.settled_wait_emissions, rf.unsettled_steps, rf.unsettled_steps_unheld,
                            rf.reattach, rf.entry_revalidate));
    t.push_back(fmt::format("rf_guard={},{},{},{},{},{},{},{}", rf.continuity_guard_cyc, rf.continuity_guard_hold,
                            rf.continuity_guard_min_run, rf.nn_guard, rf.nn_guard_min_epochs, rf.nn_guard_tol_gf_m,
                            rf.nn_guard_max_if_m, rf.nn_guard_release));
    for (const std::string& c : rf.cascade) t.push_back("rf_cascade=" + c);
    t.push_back(fmt::format("rf_cascade_flags={},{},{}", rf.cascade_tree, rf.cascade_log, rf.cascade_rehost_keep));
    // The chain's families and event rows: they shape the tree memory.
    t.push_back("family=" + config.networkPhaseDatumEq34Family());
    for (const std::string& p : config.networkPhaseDatumEq34FamilyPairs()) t.push_back("family_pair=" + p);
    for (const std::string& r : config.networkPhaseDatumEq34EventRows()) t.push_back("event_row=" + r);
    // The step-class pairs: a pair's place in the list indexes the emitter's step baselines (ESC lines).
    for (const std::string& p : nc.disc_step_class_pairs) t.push_back("step_pair=" + p);
    return t;
}

std::uint64_t networkChainMemoryFingerprint(const GnssConfig& config) {
    std::string text;
    for (const std::string& token : networkChainMemoryFingerprintTokens(config)) {
        text += token;
        text += '\x1f';
    }
    return NetworkStateSnapshotIO::fnv1a64(text);
}

std::string networkChainMemoryWrite(const NetworkRun& run) {
    const NetworkRunContext& ctx = run.ctx;
    const NetworkSetupResult& setup = run.setup;
    const GnssConfig& config = run.config;
    // ---- End of the bindings ----
    std::ostringstream os;
    os << "CHAIN," << kChainMemoryVersion << ',' << fingerprintText(networkChainMemoryFingerprint(config)) << '\n';
    // The latch and its window: a resumed leg takes these in place of latching over its own window.
    os << fmt::format("GW,{},{}\n", setup.window_t0_s, setup.window_t1_s);
    for (const auto& [sat, value_m] : setup.sat_gf_target_m) os << fmt::format("GD,{},{}\n", sat, value_m);
    net::chainMemoryWriteEntities(ctx.entities, os);
    // The topology ledger's run fields: whether it holds a real epoch, and that epoch's shared columns.
    os << fmt::format("CTZ,{}\n", ctx.carry.topo_seeded ? 1 : 0);
    for (const std::string& label : ctx.carry.topo_prev_cols) os << "CTC," << label << '\n';
    if (const NetworkRingFix* rf = ctx.carry.ring_fix.get()) ringFixWriteMemory(*rf, os);
    if (setup.emitter) setup.emitter->writeHeldValueState(os);
    os << "CHEND\n";
    return os.str();
}

void networkChainMemoryRestore(NetworkRun& run) {
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config = run.config;
    NetworkSetupResult& setup = run.setup;                // written: the latch
    net::NetworkEntities& entities = ctx.entities;        // written (the arcs, lines of sight, satellites, satellite bands, stations)
    bool& topo_seeded = ctx.carry.topo_seeded;            // written: the topology ledger
    std::set<std::string>& topo_prev_cols = ctx.carry.topo_prev_cols;   // written: the topology ledger
    bool& pin_lives = ctx.carry.resume.chain_pin_lives;   // written: the restored pinned lives' first election
    std::string& staged = ctx.carry.resume.chain_memory;  // written: consumed here
    auto& counters = ctx.counters;                         // written: the restore's counters
    const std::shared_ptr<spdlog::logger>& log = run.log;
    // ---- End of the bindings ----
    if (staged.empty()) return;
    const std::string text = staged;
    staged.clear();
    // The header: the format version and the fingerprint of the keys that shape the memory.
    const std::string header = text.substr(0, text.find('\n'));
    const std::vector<std::string> h = splitFields(header);
    const std::string live = fingerprintText(networkChainMemoryFingerprint(config));
    std::string refusal;
    if (h.size() != 3 || h[0] != "CHAIN" || h[1] != kChainMemoryVersion)
        refusal = fmt::format("its header '{}' is not this build's ('CHAIN,{},<fingerprint>')", header,
                              kChainMemoryVersion);
    else if (h[2] != live)
        refusal = fmt::format("its fingerprint {} does not match this run's {}: the keys that shape the memory (the "
                              "ring fix's, the chain's family, family pairs and event rows, the step-class pairs) "
                              "differ", h[2], live);
    if (!refusal.empty()) {
        ++counters.chain_restore_refused;
        log->warn("Network (chain restore): the snapshot's chain memory section is REFUSED: {}.  Nothing of it is "
                  "restored, and the warm resume goes on without it (the chain's tree memory and the ring fix start "
                  "empty and the leg latches its own group delays).", refusal);
        return;
    }
    ++counters.chain_restore_restored;
    std::map<std::string, double> latch;
    double window_t0_s = 0.0, window_t1_s = 0.0;
    bool have_window = false;
    long long n_entity = 0, n_ring = 0, n_emitter = 0, n_unknown = 0;
    std::string first_unknown;
    NetworkRingFix* rf = ctx.carry.ring_fix.get();
    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line == "CHEND" || line.rfind("CHAIN,", 0) == 0) continue;
        const std::vector<std::string> f = splitFields(line);
        const std::string& tag = f[0];
        if (tag == "GW" && f.size() == 3) {
            window_t0_s = std::stod(f[1]);
            window_t1_s = std::stod(f[2]);
            have_window = true;
        } else if (tag == "GD" && f.size() == 3) {
            latch[f[1]] = std::stod(f[2]);
        } else if (tag == "CTZ" && f.size() == 2) {
            topo_seeded = f[1] == "1";
            ++n_entity;
        } else if (tag == "CTC" && f.size() == 2) {
            topo_prev_cols.insert(f[1]);
            ++n_entity;
        } else if (!tag.empty() && tag[0] == 'C' && net::chainMemoryReadEntityLine(entities, f)) {
            ++n_entity;
        } else if (!tag.empty() && tag[0] == 'R' && rf != nullptr && ringFixReadMemoryLine(*rf, f)) {
            ++n_ring;
        } else if (!tag.empty() && tag[0] == 'E' && setup.emitter && setup.emitter->readHeldValueStateLine(f)) {
            ++n_emitter;
        } else {
            if (n_unknown++ == 0) first_unknown = line;
        }
    }
    counters.chain_restore_lines += n_entity + n_ring + n_emitter + static_cast<long long>(latch.size())
                                  + (have_window ? 1 : 0);
    counters.chain_restore_unread += n_unknown;
    // The latch: the leg uses the saved values in place of its own, which it latched over its own window.
    long long n_changed = 0;
    double largest_change_m = 0.0;
    for (const auto& [sat, value_m] : latch) {
        const auto own = setup.sat_gf_target_m.find(sat);
        const double d = own == setup.sat_gf_target_m.end() ? std::abs(value_m) : std::abs(value_m - own->second);
        if (d > 0.0) {
            ++n_changed;
            largest_change_m = std::max(largest_change_m, d);
        }
    }
    const double own_t0_s = setup.window_t0_s, own_t1_s = setup.window_t1_s;
    if (!latch.empty() || have_window) setup.sat_gf_target_m = latch;
    // The pinned lives: the first election judges each on this run's own ledger and flags.
    long long n_lives = 0;
    net::forEach(entities.arcs, &net::ArcState::eq34_pin_life, [&](const ArcId&, const ArcLifeStamp&) { ++n_lives; });
    pin_lives = n_lives > 0;
    long long n_held = 0;
    if (rf != nullptr)
        for (const auto& [id, L] : rf->links)
            if (L.held) ++n_held;
    log->info("Network (chain restore): the chain memory section restored at the resume (fingerprint {}): {} entity "
              "line(s) (the vertices seen in earlier trees, the last tree roots, the root targets, {} pinned life(s) "
              "for the first election to judge, the anchor ages, the carries' re-level stamps, the satellites' "
              "code-reference events, the emitter's anchor baseline, the topology ledger), {} ring fix line(s) ({} "
              "link(s), {} held), {} emitter line(s); the group-delay latch of {} satellite(s) latched over [TOW "
              "{:.1f}, TOW {:.1f}) replaces this leg's own over [TOW {:.1f}, TOW {:.1f}) ({} satellite(s) differ from "
              "it, by up to {:.4f} m).",
              live, n_entity, n_lives, n_ring, rf != nullptr ? rf->links.size() : 0, n_held, n_emitter, latch.size(),
              towOf(window_t0_s), towOf(window_t1_s), towOf(own_t0_s), towOf(own_t1_s), n_changed, largest_change_m);
    if (n_unknown > 0)
        log->warn("Network (chain restore): {} line(s) of the chain memory section were not read (the first: '{}'): "
                  "a line for a part this run does not run (the ring fix, or a leveling carry the resume did not "
                  "restore).", n_unknown, first_unknown);
}

}  // namespace GnssProcessor
