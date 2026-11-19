/**
 * @file ChainMemory.cpp
 * @brief The chain memory section of the snapshot: its writer and its restore (network.state_snapshot_chain_capture,
 *        network.resume_chain_restore).  The format and the parts are described in the header.
 */
#include "network/state/ChainMemory.hpp"

#include "network/ar/RingFix.hpp"
#include "network/driver/Run.hpp"
#include "network/state/Entities.hpp"
#include "network/state/RunContext.hpp"
#include "products/NetworkProductEmitter.hpp"

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

}  // namespace

std::string networkChainMemoryWrite(const NetworkRun& run) {
    const NetworkRunContext& ctx = run.ctx;
    const NetworkSetupResult& setup = run.setup;
    // ---- End of the bindings ----
    std::ostringstream os;
    os << "CHAIN,v1\n";
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
    NetworkSetupResult& setup = run.setup;                // written: the latch
    net::NetworkEntities& entities = ctx.entities;        // written (the arcs, lines of sight, satellite bands, stations)
    bool& topo_seeded = ctx.carry.topo_seeded;            // written: the topology ledger
    std::set<std::string>& topo_prev_cols = ctx.carry.topo_prev_cols;   // written: the topology ledger
    const std::shared_ptr<spdlog::logger>& log = run.log;
    std::string& staged = ctx.carry.resume.chain_memory;  // written: consumed here
    // ---- End of the bindings ----
    if (staged.empty()) return;
    std::map<std::string, double> latch;
    double window_t0_s = 0.0, window_t1_s = 0.0;
    bool have_window = false;
    long long n_entity = 0, n_ring = 0, n_emitter = 0, n_unknown = 0;
    std::string first_unknown;
    NetworkRingFix* rf = ctx.carry.ring_fix.get();
    std::istringstream is(staged);
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
    staged.clear();
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
    long long n_held = 0;
    if (rf != nullptr)
        for (const auto& [id, L] : rf->links)
            if (L.held) ++n_held;
    log->info("Network (chain restore): the chain memory section restored at the resume: {} entity line(s) (the "
              "vertices seen in earlier trees, the last tree roots, the root targets, the pinned lives, the anchor "
              "ages, the emitter's anchor baseline, the topology ledger), {} ring fix line(s) ({} link(s), {} held), "
              "{} emitter line(s); "
              "the group-delay latch of {} satellite(s) latched over [TOW {:.1f}, TOW {:.1f}) replaces this leg's own "
              "over [TOW {:.1f}, TOW {:.1f}) ({} satellite(s) differ from it, by up to {:.4f} m).",
              n_entity, n_ring, rf != nullptr ? rf->links.size() : 0, n_held, n_emitter, latch.size(),
              towOf(window_t0_s), towOf(window_t1_s), towOf(own_t0_s), towOf(own_t1_s), n_changed,
              largest_change_m);
    if (n_unknown > 0)
        log->warn("Network (chain restore): {} line(s) of the chain memory section were not read (the first: '{}'); "
                  "a ring fix line without the ring fix on this run, or a section written by another build.",
                  n_unknown, first_unknown);
}

}  // namespace GnssProcessor
