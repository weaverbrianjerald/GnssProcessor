/**
 * @file test_eq34_reattach_hygiene.cpp
 * @brief eq34 REATTACH HYGIENE (docs/SPEC_eq34_reattach_hygiene.md) — the anchor-host
 *        eligibility gate, the INC-2 time-linked "pin bridge" lifecycle, and the
 *        INC-3 fresh-pin σ ramp.
 *
 * What is worth testing here is what can go wrong SILENTLY.  The field defect this
 * card fixes produced a completely clean epoch (post-fit σ̂0 2.95) and only broke one
 * epoch LATER, so none of the run-time screens could attribute it; the guard is
 * therefore a pure ELECTION decision, and an election decision that quietly does
 * nothing looks exactly like one that works.  Every case below pins a decision that
 * has no other observable:
 *
 *   1. a mature host must OUTRANK a fresh one — regardless of the coverage / I4
 *      longevity order the election would otherwise follow;
 *   2. with only fresh candidates the key must be HELD OUT (and counted), not pinned;
 *   3. armed with INC-2 the same situation must open a BRIDGE at the last solved
 *      level instead, and the bridge must expire at its staleness bound;
 *   4. a slip on the bridged key must DROP the bridge (standing invariant #1 — a
 *      stiff time-linked quantity with no wired event response is the failure mode
 *      this project has paid for twice);
 *   5. a warm resume must seed ages MATURE, or a resumed leg blocks every reattach;
 *   6. OFF must be structurally inert — no age map, no bridge, no counter, and an
 *      election whose pin set is identical to the pre-card one.
 *
 * The eq34 chain election is exercised directly (it reads only station NAMES and the
 * candidate census, never a StateManager), while the hygiene seam gets real
 * StateManagers because it prunes phase-bias states and reads pending-reseed flags.
 */

#include <gtest/gtest.h>

#include "app/NetworkRunContext.hpp"
#include "config/GnssConfig.hpp"
#include "filter/NetworkAnchorHygiene.hpp"
#include "filter/NetworkChainDatum.hpp"
#include "filter/NetworkRunTypes.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"
#include "math/TimeUtils.hpp"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

constexpr std::size_t kPppCoreSize = 5;
constexpr const char* kSig = "L1C";

/// Empty C3c slip funnel — every test in THIS file is about reattach hygiene, not slips,
/// so the election's slip-aware path (network.phase_datum_eq34_slip_aware_election) must
/// see nothing.  A named constant rather than a temporary so it cannot be mistaken for a
/// fixture that some test quietly populates.
const std::set<std::pair<int, std::string>> kNoSlips;

fs::path writeTempConfig(const std::string& tag, const std::string& body) {
    const fs::path p = fs::temp_directory_path() / ("eq34hyg_cfg_" + tag + ".json");
    std::ofstream f(p);
    f << body;
    f.close();
    return p;
}

/// A minimal loadable PPP-RTK network config on the eq34 chain datum, with @p net_extra
/// spliced into the network block.
std::string networkConfig(const std::string& net_extra) {
    return std::string("{\n")
        + "  \"processing\": {\n"
          "    \"mode\": \"ppp-rtk-network\",\n"
          "    \"active_systems\": [ \"GPS\" ],\n"
          "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
          "  },\n"
          "  \"network\": {\n"
          "    \"clock_datum_station\": \"AAAA\",\n"
          "    \"phase_datum_eq34\": true,\n"
        + net_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Load a config through the real loader (so every INERT-guard and clamp runs) and
/// hand back the owned GnssConfig.
std::unique_ptr<GnssConfig> loadCfg(LogManager& log, const std::string& tag,
                                    const std::string& net_extra) {
    auto cfg = std::make_unique<GnssConfig>();
    const fs::path p = writeTempConfig(tag, networkConfig(net_extra));
    const bool ok = cfg->loadFromJson(p, log);
    std::error_code ec; fs::remove(p, ec);
    EXPECT_TRUE(ok) << "loadFromJson failed for tag " << tag;
    return cfg;
}

/// Arm INC-1 (and optionally INC-2 / INC-3) in the network block.
std::string armed(int min_arc_ep, bool bridge = false, int bridge_max = 20,
                  double fresh_scale = 1.0, int fresh_ramp = 0) {
    std::string s = "    \"eq34_anchor_min_arc_ep\": " + std::to_string(min_arc_ep) + ",\n";
    if (bridge) {
        s += "    \"phase_datum_time_linked\": true,\n";
        s += "    \"phase_datum_bridge_max_ep\": " + std::to_string(bridge_max) + ",\n";
    }
    if (fresh_scale > 1.0) {
        s += "    \"eq34_fresh_pin_sigma_scale\": " + std::to_string(fresh_scale) + ",\n";
        s += "    \"eq34_fresh_pin_ramp_ep\": " + std::to_string(fresh_ramp) + ",\n";
    }
    return s;
}

/// Per-station census entry: which satellites this station tracks on kSig.
struct StationSpec {
    std::string name;
    std::vector<std::string> sats;
    int coverage = 0;      ///< the election's per-constellation ranking key (higher wins)
};

/// Build the pieces the election consumes: NetStation names, the presence mask and the
/// PhaseDatumCandidates census.  Stations get NO StateManager — the chain election never
/// touches one (see the file banner); the hygiene tests below add real ones.
struct Census {
    std::vector<NetStation> stations;
    std::vector<bool> present;
    PhaseDatumCandidates cnd;
};

Census makeCensus(const std::vector<StationSpec>& spec) {
    Census c;
    c.stations.resize(spec.size());
    c.present.assign(spec.size(), true);
    c.cnd.sat_el.resize(spec.size());
    c.cnd.usable_keys.resize(spec.size());
    for (std::size_t i = 0; i < spec.size(); ++i) {
        c.stations[i].name = spec[i].name;
        c.cnd.cover['G'][i] = spec[i].coverage;
        c.cnd.cand_set['G'].insert(i);
        for (const auto& sat : spec[i].sats) {
            const std::string amb = sat + "_" + kSig;
            c.cnd.sat_el[i][sat] = 45.0;
            c.cnd.usable_keys[i][amb] = StateKeys::makeSatPhaseBiasKey(sat, kSig);
            c.cnd.union_keys[amb] = StateKeys::makeSatPhaseBiasKey(sat, kSig);
        }
    }
    return c;
}

/// Attach a real StateManager holding one satellite phase-bias state and one ambiguity
/// per tracked satellite — what the hygiene seam actually reads and prunes.
void attachStateManagers(Census& c, const std::vector<StationSpec>& spec,
                         LogManager& log, const GnssConfig& cfg) {
    for (std::size_t i = 0; i < spec.size(); ++i) {
        c.stations[i].sm = std::make_unique<StateManager>(kPppCoreSize, log, cfg);
        for (const auto& sat : spec[i].sats) {
            c.stations[i].sm->addSatPhaseBiasState(
                StateKeys::makeSatPhaseBiasKey(sat, kSig), 0.0);
            c.stations[i].sm->addAmbiguity(StateKeys::makeAmbiguityKey(sat, kSig), 0.0);
        }
    }
}

GnssTime epochAt(double tow) {
    GnssTime t;
    t.week = 2314;
    t.tow  = tow;
    return t;
}

/// Give a key a FRESH emitted-realization baseline at @p tow.  The eligibility block is
/// warm-conditioned (spec addendum): without a recent realization there is no warm
/// memory for a cold pin to fight, so the guard deliberately stands down.  Every
/// "should block" scenario therefore has to establish this first — which is also the
/// honest field shape: the network was emitting the key seconds before it re-hosted.
void markRealizedAt(NetworkRunContext& ctx, const std::string& amb, double tow) {
    ctx.carry.emitted_anchor_s[amb] = TimeUtils::continuousGpsSeconds(epochAt(tow));
}

/// Which station (if any) holds the tree-edge pin for @p amb?
std::string pinnedHost(const Census& c, const std::string& amb) {
    for (const auto& st : c.stations)
        if (st.pinned_amb_targets.count(amb)) return st.name;
    return {};
}

/// EVERY tree edge of the elected spanning tree, as (station index, ambiguity key).
/// `pinnedHost` returns only the first station holding a given key, which is lossy: a
/// satellite vertex of degree 2 is pinned at BOTH its tree neighbours, and the slip-aware
/// election's whole question is which of them kept the edge.
std::set<std::pair<int, std::string>> treeEdges(const Census& c) {
    std::set<std::pair<int, std::string>> out;
    for (std::size_t i = 0; i < c.stations.size(); ++i)
        for (const auto& [amb, target] : c.stations[i].pinned_amb_targets)
            out.insert({static_cast<int>(i), amb});
    return out;
}

/// Run the hygiene seam for one epoch.  @p slipped is the PASS-A funnel.
AnchorHygiene runHygiene(NetworkRunContext& ctx, const GnssConfig& cfg, Census& c,
                         double tow,
                         const std::set<std::pair<int, std::string>>& slipped = {}) {
    LogManager lm;
    NetworkRunTraces traces(fs::temp_directory_path());   // no stream opened ⇒ no I/O
    AnchorHygiene hyg;
    networkAnchorHygiene(ctx, cfg, spdlog::default_logger(), traces, epochAt(tow),
                         c.stations, c.present, slipped, c.cnd, hyg);
    return hyg;
}

/// Age an arc to @p n epochs by running the seam n times on a stable census.
void ageArcs(NetworkRunContext& ctx, const GnssConfig& cfg, Census& c, int n,
             double tow0 = 1000.0) {
    for (int k = 0; k < n; ++k) runHygiene(ctx, cfg, c, tow0 + 30.0 * k);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. A MATURE host outranks a FRESH one, whatever the election's own rank order says
// ---------------------------------------------------------------------------
//
// The defect's shape exactly: G29's incumbent host dies, and the walk would otherwise
// take whichever host its coverage / attachment order reaches first — at DOY-129 that
// was INEG's one-epoch-old arc.  With INC-1 armed the walk's FIRST pass admits only
// converged arcs, so the mature host must win even though the fresh one is ranked
// above it (higher coverage) and sorts first by station index.
TEST(Eq34ReattachHygiene, MatureHostPreferredOverColdHostRegardlessOfRank) {
    LogManager log;
    auto cfg = loadCfg(log, "prefer_mature", armed(/*min_arc_ep=*/5));
    ASSERT_GT(cfg->getNetworkConfig().eq34_anchor_min_arc_ep, 0);

    // FRESH (index 0, coverage 99 — ranked first by every legacy criterion) and MATURE
    // (index 1, coverage 1).  Both track G29; a third satellite keeps both stations in
    // the same connected component so the walk really has a choice.
    const std::vector<StationSpec> spec = {
        { "FRSH", { "G29", "G01" }, 99 },
        { "MATR", { "G29", "G01" },  1 },
    };
    Census c = makeCensus(spec);
    attachStateManagers(c, spec, log, *cfg);

    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.anchor_hygiene_on);

    // Age BOTH arcs mature, then make FRSH's G29 arc restart (a slip refloats it), so
    // MATR is the only converged host for G29.  The incumbent host is GONE — its arc
    // died, which is exactly what forces the reattach (the incumbent itself is
    // deliberately exempt from the age test; keeping it is not a reattach).
    ageArcs(ctx, *cfg, c, 6);
    ctx.carry.anchor_incumbent["G29_" + std::string(kSig)] = "GONE";
    const AnchorHygiene hyg =
        runHygiene(ctx, *cfg, c, 2000.0, { { 0, "G29_" + std::string(kSig) } });

    EXPECT_EQ(hyg.arcAge(0, "G29_L1C"), 0);   // refloated ⇒ fresh by definition
    EXPECT_GE(hyg.arcAge(1, "G29_L1C"), 5);
    EXPECT_FALSE(hyg.eligibleHost(0, "G29_L1C"));
    EXPECT_TRUE(hyg.eligibleHost(1, "G29_L1C"));
    // A converged host exists, so the key is NOT held out.
    EXPECT_EQ(c.cnd.union_keys.count("G29_L1C"), 1u);
    EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 0);

    std::set<std::string> shared;
    networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), c.stations,
                           c.present, c.cnd, hyg, kNoSlips, shared);
    EXPECT_EQ(pinnedHost(c, "G29_L1C"), "MATR")
        << "the election took the FRESH host despite a converged one being available "
           "— this is the DOY-129 19:25:30 mechanism";
}

// ---------------------------------------------------------------------------
// 2. Only-fresh candidates ⇒ the key is HELD OUT, its states pruned, and counted
// ---------------------------------------------------------------------------
TEST(Eq34ReattachHygiene, ColdOnlyCandidatesBlockTheReattachAndPruneTheKey) {
    LogManager log;
    auto cfg = loadCfg(log, "block_cold", armed(/*min_arc_ep=*/5));

    const std::vector<StationSpec> spec = {
        { "AAAA", { "G29", "G01" }, 10 },
        { "BBBB", { "G29", "G01" },  9 },
    };
    Census c = makeCensus(spec);
    attachStateManagers(c, spec, log, *cfg);

    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    // The incumbent is a station that is NOT present at all — the arc died, which is
    // precisely what forces a reattach.  Both live arcs are one epoch old.
    ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";
    markRealizedAt(ctx, "G29_L1C", 970.0);   // emitted one epoch ago: warm memory exists
    const AnchorHygiene hyg = runHygiene(ctx, *cfg, c, 1000.0);

    EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 1);
    EXPECT_EQ(c.cnd.union_keys.count("G29_L1C"), 0u) << "blocked key must leave the union";
    EXPECT_EQ(c.cnd.union_keys.count("G01_L1C"), 1u) << "a first sighting is never gated";
    // The phase-bias STATES of the blocked key are pruned at every tracking station, so
    // the per-station column sets stay consistent with the gated union (the coverage
    // gate's disposition, byte for byte).
    for (const auto& st : c.stations) {
        EXPECT_EQ(st.sm->getSatPhaseBiasStates().count(
                      StateKeys::makeSatPhaseBiasKey("G29", kSig)), 0u);
        EXPECT_EQ(st.sm->getSatPhaseBiasStates().count(
                      StateKeys::makeSatPhaseBiasKey("G01", kSig)), 1u);
    }

    std::set<std::string> shared;
    networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), c.stations,
                           c.present, c.cnd, hyg, kNoSlips, shared);
    EXPECT_TRUE(pinnedHost(c, "G29_L1C").empty()) << "a held-out key must not be pinned";
    EXPECT_EQ(shared.count(StateKeys::makeSatPhaseBiasKey("G29", kSig)), 0u);
    EXPECT_EQ(shared.count(StateKeys::makeSatPhaseBiasKey("G01", kSig)), 1u);
}

// ---------------------------------------------------------------------------
// 2b. The hold-out must PERSIST until a host converges — a one-epoch block is a bug
// ---------------------------------------------------------------------------
//
// A held-out key is absent from this_epoch_anchor, so it also drops out of
// anchor_incumbent at the end of the election.  Without a persistent record it would
// read as a FIRST SIGHTING on the very next epoch — and first sightings are
// deliberately never gated — so the guard would refuse the cold pin once and then
// grant it one epoch later, on an arc one epoch older and still code-seeded.  The hold
// must last until an arc actually reaches the threshold, and then release.
TEST(Eq34ReattachHygiene, HoldOutPersistsUntilAHostConvergesThenReleases) {
    LogManager log;
    constexpr int kMin = 4;
    auto cfg = loadCfg(log, "hold_persist", armed(kMin));
    const std::vector<StationSpec> spec = {
        { "AAAA", { "G29", "G01" }, 10 },
        { "BBBB", { "G29", "G01" },  9 },
    };
    Census c = makeCensus(spec);
    attachStateManagers(c, spec, log, *cfg);
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";
    markRealizedAt(ctx, "G29_L1C", 1000.0);

    // Epoch 1 blocks.  Then, epoch after epoch, the key must STAY out even though the
    // incumbent has long since been forgotten — until the arcs reach kMin epochs.
    for (int ep = 1; ep < kMin; ++ep) {
        Census ce = makeCensus(spec);
        attachStateManagers(ce, spec, log, *cfg);
        const AnchorHygiene hyg = runHygiene(ctx, *cfg, ce, 1000.0 + 30.0 * ep);
        EXPECT_EQ(ce.cnd.union_keys.count("G29_L1C"), 0u)
            << "released at epoch " << ep << " with every host still cold";
        EXPECT_EQ(hyg.arcAge(0, "G29_L1C"), ep);
        // The incumbent really is gone — the hold is what is doing the work.
        EXPECT_EQ(ctx.carry.anchor_incumbent.count("G29_L1C"), 1u);
    }
    EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 1) << "one HOLD, not one per epoch";
    EXPECT_EQ(ctx.counters.hyg.blocked_key_epochs, kMin - 1);
    EXPECT_EQ(ctx.counters.hyg.blocks_released, 0);

    // The epoch the arcs reach the threshold, the hold releases and the key returns.
    Census cr = makeCensus(spec);
    attachStateManagers(cr, spec, log, *cfg);
    const AnchorHygiene hyg = runHygiene(ctx, *cfg, cr, 2000.0);
    EXPECT_EQ(hyg.arcAge(0, "G29_L1C"), kMin);
    EXPECT_EQ(cr.cnd.union_keys.count("G29_L1C"), 1u);
    EXPECT_EQ(ctx.counters.hyg.blocks_released, 1);
    EXPECT_EQ(ctx.carry.anchor_blocked.count("G29_L1C"), 0u);

    std::set<std::string> shared;
    networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), cr.stations,
                           cr.present, cr.cnd, hyg, kNoSlips, shared);
    EXPECT_FALSE(pinnedHost(cr, "G29_L1C").empty()) << "the key must come back";
}

// ---------------------------------------------------------------------------
// 2c. WARM CONDITIONING (spec addendum): a cold host is only refused while a fresh
//     emitted realization contradicts it
// ---------------------------------------------------------------------------
//
// The failure mode this closes is the opposite of the card's: an ABSOLUTE eligibility
// rule turns every network restart into a self-inflicted min_arc_ep-epoch phase-bias
// outage, because on a restart EVERY host is fresh and there is nothing for the guard
// to protect.  A cold pin is dangerous strictly in proportion to the warm memory it has
// to fight.  Both directions are pinned here, and the permissive one is counted rather
// than silent (it is the one that lets a cold pin through).
TEST(Eq34ReattachHygiene, ColdHostIsOnlyRefusedAgainstAFreshEmittedRealization) {
    LogManager log;
    const std::vector<StationSpec> spec = {
        { "AAAA", { "G29", "G01" }, 10 },
        { "BBBB", { "G29", "G01" },  9 },
    };
    // (a) NO baseline at all — a cold start / restart.  Nothing to shock ⇒ the key must
    //     stay in the union and converge the legacy way.
    {
        auto cfg = loadCfg(log, "warm_none", armed(/*min_arc_ep=*/5));
        Census c = makeCensus(spec);
        attachStateManagers(c, spec, log, *cfg);
        NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
        ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";   // an incumbent, but never emitted
        runHygiene(ctx, *cfg, c, 1000.0);
        EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 0);
        EXPECT_GT(ctx.counters.hyg.reattach_cold_allowed_no_prior, 0);
        EXPECT_EQ(c.cnd.union_keys.count("G29_L1C"), 1u);
    }
    // (b) STALE baseline — an outage longer than the horizon.  The carried state has
    //     expired through its own staleness machinery, so the block lifts.
    {
        auto cfg = loadCfg(log, "warm_stale",
                           armed(/*min_arc_ep=*/5)
                           + "    \"eq34_prior_realization_max_gap_s\": 600.0,\n");
        ASSERT_DOUBLE_EQ(cfg->getNetworkConfig().eq34_prior_realization_max_gap_s, 600.0);
        Census c = makeCensus(spec);
        attachStateManagers(c, spec, log, *cfg);
        NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
        ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";
        markRealizedAt(ctx, "G29_L1C", 1000.0 - 900.0);   // 900 s ago > 600 s horizon
        runHygiene(ctx, *cfg, c, 1000.0);
        EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 0);
        EXPECT_GT(ctx.counters.hyg.reattach_cold_allowed_no_prior, 0);
        EXPECT_EQ(c.cnd.union_keys.count("G29_L1C"), 1u);
    }
    // (c) FRESH baseline — the original DOY-129 case.  Note there is NO incumbent here:
    //     a satellite returning after a gap has already dropped out of the (wholesale-
    //     replaced) incumbent map, so the emitted-realization baseline is the ONLY thing
    //     that can recognise it as a re-host rather than a first sighting.
    {
        auto cfg = loadCfg(log, "warm_fresh", armed(/*min_arc_ep=*/5));
        Census c = makeCensus(spec);
        attachStateManagers(c, spec, log, *cfg);
        NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
        markRealizedAt(ctx, "G29_L1C", 970.0);            // emitted one epoch ago
        runHygiene(ctx, *cfg, c, 1000.0);
        EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 1);
        EXPECT_EQ(c.cnd.union_keys.count("G29_L1C"), 0u);
        EXPECT_EQ(c.cnd.union_keys.count("G01_L1C"), 1u)
            << "a key with no realization history is still a first sighting";
    }
}

// ---------------------------------------------------------------------------
// 3. INC-2 armed ⇒ the same situation BRIDGES at the last solved level, and the bridge
//    expires at phase_datum_bridge_max_ep
// ---------------------------------------------------------------------------
TEST(Eq34ReattachHygiene, BridgeHoldsTheSolvedLevelAndExpiresAtItsStalenessBound) {
    LogManager log;
    // min_arc_ep deliberately far above the epochs this test runs, so no candidate can
    // become eligible mid-test and "heal" the bridge — the LIFETIME rule is what is
    // under test here, and a heal would mask an expiry that never fires.
    auto cfg = loadCfg(log, "bridge", armed(/*min_arc_ep=*/50, /*bridge=*/true,
                                            /*bridge_max=*/3));
    ASSERT_TRUE(cfg->getNetworkConfig().phase_datum_time_linked);

    const std::vector<StationSpec> spec = {
        { "AAAA", { "G29", "G01" }, 10 },
        { "BBBB", { "G29", "G01" },  9 },
    };
    Census c = makeCensus(spec);
    attachStateManagers(c, spec, log, *cfg);

    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ASSERT_TRUE(ctx.knobs.pin_bridge_on);
    ASSERT_GT(ctx.knobs.pin_bridge_info, 0.0);
    ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";
    markRealizedAt(ctx, "G29_L1C", 970.0);
    ctx.carry.pb_level_prev["G29_L1C"] = -1.234;   // the harvested solved level

    AnchorHygiene hyg = runHygiene(ctx, *cfg, c, 1000.0);
    ASSERT_EQ(ctx.counters.hyg.bridges_opened, 1);
    EXPECT_TRUE(hyg.isBridged("G29_L1C"));
    EXPECT_EQ(ctx.carry.pin_bridge.at("G29_L1C").value, -1.234);
    // Bridging REPLACES the drop: the key stays in the shared set (its column is solved),
    // it is simply not pinned.
    EXPECT_EQ(c.cnd.union_keys.count("G29_L1C"), 1u);
    EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 0);

    std::set<std::string> shared;
    networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), c.stations,
                           c.present, c.cnd, hyg, kNoSlips, shared);
    // No HOST is elected for a bridged key — its datum is the shared row, and it enters
    // the walk as a spanning-forest root.  (Receivers reached THROUGH it still pin their
    // own z^s_r on that satellite; that is the edge which determines δ_r, and it is
    // subject to the same eligibility test.)
    EXPECT_EQ(ctx.carry.this_epoch_anchor.count("G29_L1C"), 0u)
        << "a bridged key must not be re-hosted";
    EXPECT_EQ(shared.count(StateKeys::makeSatPhaseBiasKey("G29", kSig)), 1u);

    // Staleness: the bridge survives while folds < max_ep and is dropped once it has
    // been folded max_ep times.  (The fold counter is bumped by the row injection; drive
    // it directly here — this test owns the LIFETIME rule, not the fold plumbing.)
    for (int k = 0; k < 3; ++k) {
        ctx.carry.pin_bridge.at("G29_L1C").folds = k;
        Census c2 = makeCensus(spec);
        attachStateManagers(c2, spec, log, *cfg);
        hyg = runHygiene(ctx, *cfg, c2, 1030.0 + 30.0 * k);
        EXPECT_TRUE(hyg.isBridged("G29_L1C")) << "dropped early at folds=" << k;
    }
    ctx.carry.pin_bridge.at("G29_L1C").folds = 3;   // == phase_datum_bridge_max_ep
    Census c3 = makeCensus(spec);
    attachStateManagers(c3, spec, log, *cfg);
    hyg = runHygiene(ctx, *cfg, c3, 1200.0);
    EXPECT_FALSE(hyg.isBridged("G29_L1C"));
    EXPECT_EQ(ctx.counters.hyg.bridge_drop_stale, 1);
    // Expiry must NOT silently re-open on the same (now time-expired) level, and with no
    // converged host the key falls back to the INC-1 hold-out.
    EXPECT_EQ(ctx.counters.hyg.bridges_opened, 1);
    EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 1);
    EXPECT_EQ(c3.cnd.union_keys.count("G29_L1C"), 0u);
}

// ---------------------------------------------------------------------------
// 4. Standing invariant #1: a slip on the bridged key DROPS the bridge
// ---------------------------------------------------------------------------
//
// The bridge is a stiff, zero-q, time-linked quantity, and this project has twice paid
// field time for one without a wired slip response (phase-leveling's zero-q carry; the
// Card-4 invariant DD carry, 10.9 cyc/ep FAIL).  A cycle slip aliases n·λ into the very
// ambiguity whose level the bridge is holding, so the held value is poison — reset, do
// not repair.  Both slip feeds must work: the Card-4 funnel AND the phase-leveling
// pending-reseed flag (they are gated by different, non-overlapping toggles).
TEST(Eq34ReattachHygiene, BridgeDropsOnASlipOfItsOwnKeyFromEitherFeed) {
    LogManager log;
    const std::vector<StationSpec> spec = {
        { "AAAA", { "G29", "G01" }, 10 },
        { "BBBB", { "G29", "G01" },  9 },
    };
    for (int feed = 0; feed < 2; ++feed) {
        auto cfg = loadCfg(log, "bridge_slip", armed(/*min_arc_ep=*/5, /*bridge=*/true));
        Census c = makeCensus(spec);
        attachStateManagers(c, spec, log, *cfg);
        NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
        ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";
        markRealizedAt(ctx, "G29_L1C", 970.0);
        ctx.carry.pb_level_prev["G29_L1C"] = 0.5;
        ASSERT_TRUE(runHygiene(ctx, *cfg, c, 1000.0).isBridged("G29_L1C"));

        Census c2 = makeCensus(spec);
        attachStateManagers(c2, spec, log, *cfg);
        std::set<std::pair<int, std::string>> funnel;
        if (feed == 0) {
            funnel.insert({ 0, "G29_L1C" });               // Card-4 funnel
        } else {
            c2.stations[0].sm->markAmbiguityPendingReseed("G29_L1C");  // leveling refloat
        }
        const AnchorHygiene hyg = runHygiene(ctx, *cfg, c2, 1030.0, funnel);

        EXPECT_FALSE(hyg.isBridged("G29_L1C")) << "feed " << feed;
        EXPECT_EQ(ctx.counters.hyg.bridge_drop_slip, 1) << "feed " << feed;
        // ...and it must not immediately re-open on the poisoned level.
        EXPECT_EQ(ctx.counters.hyg.bridges_opened, 1) << "feed " << feed;
        EXPECT_EQ(ctx.carry.pin_bridge.count("G29_L1C"), 0u) << "feed " << feed;
    }
}

// ---------------------------------------------------------------------------
// 5. Warm resume seeds ages MATURE
// ---------------------------------------------------------------------------
//
// The age map is deliberately not serialized (spec §7).  Without the seed every arc
// would look one epoch old on the first resumed epoch and the gate would block EVERY
// reattach for min_arc_ep epochs — turning a hygiene knob into an outage.  Seeding from
// PRESENCE rather than from "carried keys" is load-bearing: the stacks this card targets
// run with phase_leveling OFF, so local_carry is empty there.
TEST(Eq34ReattachHygiene, WarmResumeSeedsEveryPresentArcAsMature) {
    LogManager log;
    auto cfg = loadCfg(log, "resume", armed(/*min_arc_ep=*/7));
    const std::vector<StationSpec> spec = {
        { "AAAA", { "G29" }, 10 },
        { "BBBB", { "G29" },  9 },
    };
    Census c = makeCensus(spec);
    attachStateManagers(c, spec, log, *cfg);
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";
    markRealizedAt(ctx, "G29_L1C", 970.0);
    ctx.carry.anchor_age_resume_seed = true;      // set by networkTryWarmResume

    const AnchorHygiene hyg = runHygiene(ctx, *cfg, c, 1000.0);
    EXPECT_EQ(hyg.arcAge(0, "G29_L1C"), 7);
    EXPECT_TRUE(hyg.eligibleHost(0, "G29_L1C"));
    EXPECT_EQ(ctx.counters.hyg.reattach_blocked_cold, 0)
        << "a resumed leg must not block every reattach";
    EXPECT_FALSE(ctx.carry.anchor_age_resume_seed) << "the seed is one-shot";

    // One-shot really means one-shot: an arc appearing for the first time on the NEXT
    // epoch is fresh, not mature.
    const std::vector<StationSpec> spec2 = {
        { "AAAA", { "G29", "G07" }, 10 },
        { "BBBB", { "G29" },         9 },
    };
    Census c2 = makeCensus(spec2);
    attachStateManagers(c2, spec2, log, *cfg);
    const AnchorHygiene hyg2 = runHygiene(ctx, *cfg, c2, 1030.0);
    EXPECT_EQ(hyg2.arcAge(0, "G07_L1C"), 1);
    EXPECT_EQ(hyg2.arcAge(0, "G29_L1C"), 8);
}

// ---------------------------------------------------------------------------
// 6. OFF is structurally inert — and the INC-3 ramp only ever LOOSENS a fresh pin
// ---------------------------------------------------------------------------
TEST(Eq34ReattachHygiene, DisarmedIsStructurallyInert) {
    LogManager log;
    auto cfg = loadCfg(log, "off", /*net_extra=*/"");
    const std::vector<StationSpec> spec = {
        { "AAAA", { "G29", "G01" }, 10 },
        { "BBBB", { "G29", "G01" },  9 },
    };
    Census c = makeCensus(spec);
    attachStateManagers(c, spec, log, *cfg);
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
    EXPECT_FALSE(ctx.knobs.anchor_hygiene_on);
    EXPECT_EQ(ctx.knobs.eq34_min_arc_ep, 0);
    EXPECT_FALSE(ctx.knobs.pin_bridge_on);

    ctx.carry.anchor_incumbent["G29_L1C"] = "GONE";
    const AnchorHygiene hyg = runHygiene(ctx, *cfg, c, 1000.0);

    // No state allocated, no counter touched, no key removed.
    EXPECT_FALSE(hyg.active);
    EXPECT_TRUE(ctx.carry.anchor_arc_age.empty());
    EXPECT_TRUE(ctx.carry.pin_bridge.empty());
    EXPECT_EQ(ctx.counters.hyg.epochs, 0);
    EXPECT_EQ(ctx.counters.hyg.arcs_tracked, 0);
    EXPECT_EQ(c.cnd.union_keys.size(), 2u);
    // Every predicate degrades to the legacy answer, so the election is unconstrained.
    EXPECT_TRUE(hyg.eligibleHost(0, "G29_L1C"));
    EXPECT_FALSE(hyg.isBridged("G29_L1C"));
    EXPECT_EQ(hyg.pinSigmaScale(0, "G29_L1C"), 1.0);

    std::set<std::string> shared;
    networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), c.stations,
                           c.present, c.cnd, hyg, kNoSlips, shared);
    EXPECT_FALSE(pinnedHost(c, "G29_L1C").empty());
    for (const auto& st : c.stations)
        EXPECT_TRUE(st.pinned_amb_sigma_scale.empty())
            << "OFF must leave the pin fold on its exact 1e10 legacy weight";
}

// The INC-3 ramp is a pure function of the arc age; pin it directly (its only other
// observable is a weight deep inside the parallel assembly).  It must decay MONOTONELY
// to exactly 1 and never tighten a pin below the hard-pin weight.
TEST(Eq34ReattachHygiene, FreshPinRampDecaysMonotonelyToUnity) {
    std::map<std::pair<int, std::string>, int> age;
    AnchorHygiene hyg;
    hyg.active = true;
    hyg.min_arc_ep = 5;
    hyg.fresh_pin_scale = 10.0;
    hyg.fresh_pin_ramp_ep = 5;
    hyg.age = &age;

    double prev = 1e300;
    for (int a = 0; a <= 12; ++a) {
        age[{ 0, "G29_L1C" }] = a;
        const double s = hyg.pinSigmaScale(0, "G29_L1C");
        EXPECT_GE(s, 1.0) << "age " << a << ": the ramp may only LOOSEN a datum pin";
        EXPECT_LE(s, 10.0) << "age " << a;
        EXPECT_LE(s, prev) << "age " << a << ": the ramp must be monotone";
        prev = s;
    }
    age[{ 0, "G29_L1C" }] = 5;   // at the threshold: still fully fresh
    EXPECT_DOUBLE_EQ(hyg.pinSigmaScale(0, "G29_L1C"), 10.0);
    age[{ 0, "G29_L1C" }] = 10;  // ramp end: fully hard again
    EXPECT_DOUBLE_EQ(hyg.pinSigmaScale(0, "G29_L1C"), 1.0);
}

// A ramp scale below 1 would INVERT the row weight (a datum pin tighter than the hard
// pin, or negative information).  The loader must refuse it rather than fold nonsense.
TEST(Eq34ReattachHygiene, LoaderRefusesAnInvertedRampAndDisarmsOutsideAnAnchoredDatum) {
    LogManager log;
    {
        auto cfg = loadCfg(log, "bad_scale",
                           "    \"eq34_anchor_min_arc_ep\": 5,\n"
                           "    \"eq34_fresh_pin_sigma_scale\": 0.25,\n"
                           "    \"eq34_fresh_pin_ramp_ep\": 4,\n");
        EXPECT_DOUBLE_EQ(cfg->getNetworkConfig().eq34_fresh_pin_sigma_scale, 1.0);
        EXPECT_EQ(cfg->getNetworkConfig().eq34_anchor_min_arc_ep, 5);
    }
    {   // negative threshold clamps to the legacy 0
        auto cfg = loadCfg(log, "neg_min", "    \"eq34_anchor_min_arc_ep\": -3,\n");
        EXPECT_EQ(cfg->getNetworkConfig().eq34_anchor_min_arc_ep, 0);
    }
    {   // the ENSEMBLE datum elects no anchor at all ⇒ nothing to guard ⇒ disarm loudly
        auto cfg = std::make_unique<GnssConfig>();
        const std::string body =
            std::string("{\n")
            + "  \"processing\": { \"mode\": \"ppp-rtk-network\",\n"
              "    \"active_systems\": [ \"GPS\" ],\n"
              "    \"signal_priorities\": { \"GPS\": \"WC\" } },\n"
              "  \"network\": {\n"
              "    \"clock_datum_station\": \"AAAA\",\n"
              "    \"phase_datum_ensemble\": true,\n"
              "    \"eq34_anchor_min_arc_ep\": 5,\n"
              "    \"phase_datum_time_linked\": true,\n"
              "    \"stations\": [\n"
              "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
              "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
              "    ]\n  }\n}\n";
        const fs::path p = writeTempConfig("ensemble", body);
        ASSERT_TRUE(cfg->loadFromJson(p, log));
        std::error_code ec; fs::remove(p, ec);
        EXPECT_EQ(cfg->getNetworkConfig().eq34_anchor_min_arc_ep, 0);
        EXPECT_FALSE(cfg->getNetworkConfig().phase_datum_time_linked);
    }
}

// ---------------------------------------------------------------------------
// Card 3/4 — SLIP-AWARE ELECTION (network.phase_datum_eq34_slip_aware_election)
// ---------------------------------------------------------------------------
//
// A slipped arc that sits in the spanning TREE poisons every fundamental cycle that
// closes through it, so the whole dependent chord set refloats at once; a slipped CHORD
// costs exactly one DD (its cycle support is 1 — GATE 8 in
// tests/test_network_invariant_carry_driver.cpp).  The election is the only place that
// decides which of the two a slipped arc becomes, and today it decides BEFORE knowing:
// the sticky-tree seed re-seats every live incumbent unconditionally, and the incumbent
// queue drains ahead of the new-edge walk, so a slipped incumbent keeps its seat by
// default.  This test drives exactly that.
//
// A/B on one treatment, same census and the same warm-up: with the knob OFF the slipped
// incumbent keeps its tree edge; with it ON the edge moves and the arc becomes a chord.
// The graph is deliberately dense enough (3 stations x 3 satellites) that an alternative
// route to the satellite exists — the fail-safe branch, where no alternative exists and
// the completion re-admits the slipped arc, is asserted separately below.
TEST(Eq34ReattachHygiene, SlipAwareElectionDeseatsASlippedIncumbentTreeEdge) {
    LogManager log;
    const std::vector<StationSpec> spec = {
        { "AAAA", { "G01", "G02", "G03" }, 3 },
        { "BBBB", { "G01", "G02", "G03" }, 2 },
        { "CCCC", { "G01", "G02", "G03" }, 1 },
    };

    // Warm each arm to the SAME incumbent tree, then re-elect with one arc slipped.
    auto elect = [&](bool slip_aware, std::set<std::pair<int, std::string>>& tree1,
                     std::set<std::pair<int, std::string>>& tree2,
                     long long& deseeded, long long& readmitted,
                     const std::set<std::pair<int, std::string>>& slip) {
        auto cfg = loadCfg(log, slip_aware ? "slipelect_on" : "slipelect_off",
                           armed(/*min_arc_ep=*/0)
                           + (slip_aware
                                  ? "    \"phase_datum_eq34_slip_aware_election\": true,\n"
                                  : ""));
        ASSERT_EQ(cfg->networkPhaseDatumEq34SlipAwareElection(), slip_aware);
        Census c = makeCensus(spec);
        attachStateManagers(c, spec, log, *cfg);
        NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());
        std::set<std::string> shared;
        AnchorHygiene hyg = runHygiene(ctx, *cfg, c, 1000.0);
        networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), c.stations,
                               c.present, c.cnd, hyg, kNoSlips, shared);
        tree1 = treeEdges(c);
        // Second epoch, same census, one incumbent tree edge now slipped.
        shared.clear();
        hyg = runHygiene(ctx, *cfg, c, 1030.0, slip);
        networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), c.stations,
                               c.present, c.cnd, hyg, slip, shared);
        tree2 = treeEdges(c);
        deseeded   = ctx.counters.eq34_slip_deseeded;
        readmitted = ctx.counters.eq34_slip_readmitted;
    };

    // Discover an actual incumbent edge rather than assuming one — the walk order is an
    // implementation detail and hard-coding it would make this test a tautology.
    std::set<std::pair<int, std::string>> probe1, probe2;
    long long d0 = 0, r0 = 0;
    elect(/*slip_aware=*/false, probe1, probe2, d0, r0, {});
    ASSERT_FALSE(probe1.empty()) << "the election produced no tree at all";
    const auto victim = *probe1.begin();
    const std::set<std::pair<int, std::string>> slip{ victim };

    std::set<std::pair<int, std::string>> offT1, offT2, onT1, onT2;
    long long offDe = 0, offRe = 0, onDe = 0, onRe = 0;
    elect(/*slip_aware=*/false, offT1, offT2, offDe, offRe, slip);
    elect(/*slip_aware=*/true,  onT1,  onT2,  onDe,  onRe,  slip);

    // Both arms warmed to the SAME tree — so the epoch-2 difference is the treatment.
    EXPECT_EQ(offT1, onT1) << "the arms must warm identically; otherwise the A/B is not one "
                              "treatment";
    ASSERT_EQ(offT1.count(victim), 1u) << "the chosen victim must really be a tree edge";

    // OFF: the slipped incumbent keeps its seat, and nothing is counted (byte-identical
    // path — the counters are the invariant-#4 proof the disarmed path never ran).
    EXPECT_EQ(offT2.count(victim), 1u)
        << "control: without the knob a slipped incumbent keeps its tree edge, and every "
           "chord closing through it refloats";
    EXPECT_EQ(offDe, 0);
    EXPECT_EQ(offRe, 0);

    // ON: it is de-seeded, and with two other stations tracking the same satellite the
    // completion had somewhere else to go — so the arc is now a CHORD.
    EXPECT_GE(onDe, 1) << "the slipped incumbent must lose its sticky seed";
    EXPECT_EQ(onT2.count(victim), 0u)
        << "with an alternative route available the slipped arc must end up a chord";
    EXPECT_EQ(onRe, 0) << "nothing forced the re-admission on a graph this dense";
    // ...and the tree is still a tree: same edge count, component still spanned.
    EXPECT_EQ(onT2.size(), offT2.size())
        << "de-seating must MOVE an edge, not delete one — a smaller tree is a rank loss";
}

// The fail-safe half: when the slipped arc is a component's ONLY route to a satellite,
// the completion re-admits it and the mass refloat happens anyway.  That is correct — the
// topology forced it — but it must be COUNTED, or a nonzero de-seed count reads as "no
// slipped arc is in the tree" when one still is (standing invariant #4).
TEST(Eq34ReattachHygiene, SlipAwareElectionReadmitsAnUnavoidableSlippedTreeEdge) {
    LogManager log;
    // G09 is tracked by ONE station, so its only tree edge is (AAAA, G09) whatever the
    // election prefers.  G01 keeps the component connected.
    const std::vector<StationSpec> spec = {
        { "AAAA", { "G01", "G09" }, 3 },
        { "BBBB", { "G01" },        2 },
    };
    auto cfg = loadCfg(log, "slipelect_forced",
                       armed(/*min_arc_ep=*/0)
                       + "    \"phase_datum_eq34_slip_aware_election\": true,\n");
    Census c = makeCensus(spec);
    attachStateManagers(c, spec, log, *cfg);
    NetworkRunContext ctx(*cfg, cfg->getNetworkConfig());

    const std::set<std::pair<int, std::string>> slip{ { 0, "G09_L1C" } };
    std::set<std::string> shared;
    AnchorHygiene hyg = runHygiene(ctx, *cfg, c, 1000.0, slip);
    networkElectChainDatum(ctx, *cfg, log, spdlog::default_logger(), c.stations,
                           c.present, c.cnd, hyg, slip, shared);

    EXPECT_EQ(treeEdges(c).count({ 0, "G09_L1C" }), 1u)
        << "the only route to G09 must still be elected — dropping it would cost the "
           "satellite its datum, which is strictly worse than a mass refloat";
    EXPECT_GE(ctx.counters.eq34_slip_readmitted, 1)
        << "and the unavoidable re-admission must be COUNTED, not silent";
}
