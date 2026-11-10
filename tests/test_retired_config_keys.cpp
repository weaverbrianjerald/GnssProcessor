/**
 * @file test_retired_config_keys.cpp
 * @brief The tombstone of the retired configuration keys (config/RetiredConfigKeys.hpp): its
 *        rows against the deletion they record, the scan, and the loader's warnings.
 *
 * On 2026-10-14, 150 network keys were deleted from the loader with their code.  The loader
 * ignores a key it does not know, so a configuration that still set one ran on the code as it
 * now stands with nothing in the log.  The tombstone names each such key once.  Locked here:
 *
 *  1. **The rows are the deletion.**  They equal the REMOVE rows of the ledger the deletion
 *     worked from (main 4bdd693) plus the keys deleted beside them, family by family in the
 *     commit order and counts of the deletion's record.  No row names a key the current ledger
 *     lists as read, the two current goldens set none, and the retired GPS-only field golden
 *     sets 28.
 *  2. **The scan reads what the old loader read.**  A key is found under the dotted path or
 *     the flat top-level spelling, once, with the value the old loader would have taken; the
 *     station member once for the roster.  A live key, a near miss, a flat spelling below the
 *     top level and a dotted name used as one literal key are silent.
 *  3. **The loader warns and does nothing else.**  A configuration with retired keys logs one
 *     warning per key and otherwise exactly the lines it logs without them.
 */

#include <gtest/gtest.h>

#include "config/GnssConfig.hpp"
#include "config/RetiredConfigKeys.hpp"
#include "logging/LogManager.hpp"
#include "network/ConfigLoadCapture.hpp"   // TestSupport::networkConfig: a loadable network body
#include "support/TestTempDir.hpp"         // per-process, per-test scratch directory

#include <nlohmann/json.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

// The knob of every row with verdict REMOVE in the configuration ledger at main 4bdd693, the
// ledger the deletion worked from (git show 4bdd693:docs/CONFIG_promotion_ledger.json, blob
// c8d31c0e), sorted.
const std::vector<std::string> kLedgerRemoveRowsAt4bdd693 = {
    "network.adaptive_iono_q",
    "network.amb_float_q_m2s",
    "network.amb_seam_reinflate_var_m2",
    "network.ambiguity_resolution.wl_cascade",
    "network.carry_coupling_joint_whiten",
    "network.carry_coupling_sh_marginalize",
    "network.carry_invariant_honesty",
    "network.carry_invariant_innov_refold",
    "network.carry_iono",
    "network.clock_level_carry",
    "network.clock_level_confirm_epochs",
    "network.clock_level_datum_pause",
    "network.clock_level_free_cb_follow",
    "network.clock_level_free_cb_rebase",
    "network.clock_level_horizon_s",
    "network.clock_level_pin_pair_read",
    "network.clock_level_pivot_ref",
    "network.clock_level_sigma_floor_m",
    "network.clock_signal_basis_hold",
    "network.code_bias_gf_datum",
    "network.code_bias_gf_datum_per_family",
    "network.cycle_slip.refloat_band_scope",
    "network.cycle_slip.relevel_bridge_ewl",
    "network.cycle_slip.relevel_bridge_ewl_sigma_floor_cyc",
    "network.cycle_slip.relevel_lane_snap",
    "network.cycle_slip.relevel_retap_corroborate",
    "network.cycle_slip.relevel_retap_statistical",
    "network.cycle_slip.relevel_retap_streak_max",
    "network.cycle_slip.relevel_retap_var_m2",
    "network.disc_jump_test_decommon",
    "network.emit_clock_rate",
    "network.emit_pb_filter",
    "network.emit_pb_filter_emit_sigma",
    "network.emit_pb_filter_gate_v2",
    "network.emit_pb_filter_jump_k",
    "network.emit_pb_filter_max_gap_s",
    "network.emit_pb_filter_meas_sigma_m",
    "network.emit_pb_filter_q",
    "network.emit_pb_filter_window_epochs",
    "network.emit_pb_hold_smoothed_block",
    "network.external_sat_code_bias",
    "network.fast_clock.allow_single_band",
    "network.fast_clock.emit",
    "network.fast_clock.emit_max_gap_s",
    "network.fast_clock.enable",
    "network.fast_clock.min_sats_per_station",
    "network.fast_clock.min_stations_per_sat",
    "network.fast_clock.screen_k_sigma",
    "network.fast_clock.shadow",
    "network.fast_clock.trace",
    "network.fast_clock.trace_sat_stride",
    "network.iono_carry_max_gap_s",
    "network.iono_carry_q",
    "network.iono_carry_scope",
    "network.iono_carry_source",
    "network.iono_carry_var_floor",
    "network.iono_constraint_deweight_adaptive",
    "network.iono_constraint_deweight_factor",
    "network.iono_constraint_deweight_floor",
    "network.iono_constraint_deweight_min_epochs",
    "network.iono_constraint_deweight_rate_limit",
    "network.iono_constraint_deweight_sigma_hi",
    "network.iono_constraint_deweight_sigma_lo",
    "network.iono_constraint_deweight_window_epochs",
    "network.iono_dw_admit_validate_k",
    "network.iono_dw_park_bound_tecu",
    "network.iono_dw_release_floor_eps",
    "network.iono_dw_release_reseed",
    "network.iono_dw_release_validate_k",
    "network.iono_seed_field_frame",
    "network.iono_seed_field_frame_min_sats",
    "network.lane_carry.bridge_meas",
    "network.lane_carry.fold_blocked",
    "network.lane_carry.lane_meas",
    "network.lane_carry.parallel_components",
    "network.lane_carry.skip_pinned_increments",
    "network.lane_carry.skip_pinned_within_n",
    "network.mw_wl.bridge_max_sigma_cyc",
    "network.mw_wl.bridge_sample_debias",
    "network.mw_wl.debug_r_scale",
    "network.mw_wl.enable",
    "network.mw_wl.ewl_enable",
    "network.mw_wl.ewl_fix_frac_cyc",
    "network.mw_wl.ewl_fix_sigma_cyc",
    "network.mw_wl.ewl_fold_rows",
    "network.mw_wl.ewl_min_arc_epochs",
    "network.mw_wl.f6_all_blocks",
    "network.mw_wl.f6_dump",
    "network.mw_wl.f6_max_rows",
    "network.mw_wl.fix_enable",
    "network.mw_wl.fix_frac_cyc",
    "network.mw_wl.fix_sigma_cyc",
    "network.mw_wl.inject_disagree_min_cyc",
    "network.mw_wl.lane_max_abs_xlin_m",
    "network.mw_wl.min_arc_epochs",
    "network.mw_wl.uwl_cold_fix",
    "network.mw_wl.uwl_cold_min_lambda_m",
    "network.mw_wl.uwl_cold_sigma_m",
    "network.mw_wl.uwl_dual_top",
    "network.mw_wl.uwl_enable",
    "network.mw_wl.uwl_family_min_arcs",
    "network.mw_wl.uwl_family_pairs",
    "network.mw_wl.uwl_gal_top_band",
    "network.mw_wl.uwl_mid_enable",
    "network.mw_wl.uwl_sib_gate",
    "network.mw_wl.wl_fix_frac_cyc",
    "network.mw_wl.wl_fix_sigma_cyc",
    "network.mw_wl.wl_inject_enable",
    "network.parallel_reduce",
    "network.phase_bias_exclude_unmatched_antenna",
    "network.phase_datum_ensemble",
    "network.phase_datum_hold_sigma_m",
    "network.phase_leveling_conv_rms_thresh_tecu",
    "network.phase_leveling_local_block_min_age_ep",
    "network.phase_leveling_local_block_young_mode",
    "network.phase_leveling_los_scale",
    "network.phase_leveling_refloat_at_convergence",
    "network.phase_leveling_warmup_on_convergence",
    "network.phase_leveling_young_inflate_max_age_ep",
    "network.phase_leveling_young_inflate_tau_ep",
    "network.phase_leveling_young_inflate_var_m2",
    "network.prefit_fuse_require_uncheckable",
    "network.raim_carry_invariant_refloat",
    "network.raim_erase_band_scope",
    "network.resume_carry_rebase",
    "network.resume_station_restore_shdev",
    "network.rx_cb_carry_demean",
    "network.rx_cb_carry_demean_min_stations",
    "network.rx_cb_carry_demean_within",
    "network.rx_cb_carry_step_gate",
    "network.rx_cb_carry_step_k_sigma",
    "network.rx_cb_harvest_demean",
    "network.rx_cb_harvest_screen_z",
    "network.sat_if_level_datum",
    "network.serialize_sbasis_contract",
    "network.sh_gate_min_epochs",
    "network.sh_gate_rms_max_tecu",
    "network.sh_health_gate",
    "network.shared_carry_gauge_free",
    "network.state_snapshot_resume_shock_m",
    "network.stations[].fixed",
};

// The keys deleted beside them: the lane_carry rows went with the
// WL-3 carry and the mw_wl rows were orphaned by the lane layer (ledger verdicts at 4bdd693 in
// the comments); the last key had no ledger row.
const std::vector<std::string> kDeletedBesideThem = {
    "network.lane_carry.enable",                  // KEEP_OFF
    "network.lane_carry.q_wl_scale",              // PARAM
    "network.lane_carry.sigma_wl_fresh_cyc",      // PARAM
    "network.mw_wl.d5_dump",                      // TRACE
    "network.mw_wl.fix_weight_sigma_cyc",         // PARAM
    "network.mw_wl.lane_trace",                   // TRACE
    "network.mw_wl.max_rows_per_epoch",           // PARAM
    "network.mw_wl.sigma_floor_cyc",              // PARAM
    "network.iono_constraint_deweight_stations",  // no ledger row
};

// The deletion commits in order with their key counts: the family table of the deletion's
// record (docs/record/refound/RKD_retired_key_deletion_2026-10-14.md section 1, plan commits C2 to
// C29), the six orphaned keys counted under the commits that deleted them (365edfe five,
// 158b9b2 one).
const std::vector<std::pair<std::string, std::size_t>> kKeysPerCommit = {
    {"4c0e731", 7}, {"f1ea3de", 10}, {"c5be295", 11}, {"819f4a6", 8}, {"365edfe", 16},
    {"f48868c", 7}, {"1de8b9b", 10}, {"2ba604a", 1},  {"b326c8c", 9}, {"7914263", 2},
    {"6d719e4", 8}, {"08cd639", 1},  {"b216f55", 3},  {"158b9b2", 14}, {"1d35585", 1},
    {"09e3151", 3}, {"f4be1a8", 6},  {"15b927c", 9},  {"d1cd9ea", 5}, {"1068750", 3},
    {"34793c9", 3}, {"e7e470b", 3},  {"98df220", 4},  {"6e86437", 1}, {"dab7d80", 2},
    {"475b587", 1}, {"159f846", 1},  {"8409b43", 1},
};

nlohmann::json parseFile(const fs::path& p) {
    std::ifstream f(p);
    EXPECT_TRUE(f.is_open()) << p.string();
    return nlohmann::json::parse(f, /*callback=*/nullptr, /*allow_exceptions=*/true,
                                 /*ignore_comments=*/true);
}

std::vector<std::string> pathsOf(const std::vector<RetiredKeyHit>& hits) {
    std::vector<std::string> out;
    for (const RetiredKeyHit& h : hits) out.emplace_back(h.key->path);
    return out;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

/// Load @p cfg as a configuration file and return the loader's log, one "<level> <message>"
/// line per entry, with no wall-clock prefix.
std::vector<std::string> loadAndLog(const nlohmann::json& cfg, bool& ok) {
    const fs::path dir = gnss_test::tempDir();
    const fs::path cfg_path = dir / "config.json";
    {
        std::ofstream f(cfg_path);
        f << cfg.dump(2);
    }
    // Only the names this function creates are dropped (never drop_all()).
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");
    LogManager log;
    log.initialize(dir, LogLevel::Info);
    std::ostringstream oss;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    sink->set_pattern("%l %v");
    auto cfg_log = log.getLogger("gnss_config");
    auto main_log = log.getLogger();
    cfg_log->sinks().push_back(sink);
    main_log->sinks().push_back(sink);

    GnssConfig config;
    ok = config.loadFromJson(cfg_path, log);
    cfg_log->flush();
    main_log->flush();
    spdlog::drop("gnss_processor");
    spdlog::drop("gnss_config");

    std::vector<std::string> lines;
    std::istringstream in(oss.str());
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool isRetiredKeyWarning(const std::string& line) {
    return line.rfind("warning Config: retired key ", 0) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
//  1. The rows are the deletion
// ---------------------------------------------------------------------------

TEST(RetiredConfigKeys, RowsAreTheLedgerRemoveRowsAndTheKeysDeletedBesideThem) {
    std::set<std::string> expected(kLedgerRemoveRowsAt4bdd693.begin(),
                                   kLedgerRemoveRowsAt4bdd693.end());
    ASSERT_EQ(expected.size(), 141u) << "the ledger at 4bdd693 holds 141 REMOVE rows";
    for (const std::string& k : kDeletedBesideThem)
        EXPECT_TRUE(expected.insert(k).second) << k << " is also a REMOVE row";

    std::set<std::string> paths;
    for (const RetiredConfigKey& k : kRetiredConfigKeys)
        EXPECT_TRUE(paths.insert(std::string(k.path)).second) << "two rows for " << k.path;
    EXPECT_EQ(paths.size(), 150u);
    EXPECT_EQ(paths, expected);
}

TEST(RetiredConfigKeys, FamiliesFollowTheDeletionCommits) {
    // Families in commit order, each contiguous, its rows sorted by path.
    std::vector<std::pair<std::string, std::size_t>> seen;
    std::string_view previous_path;
    for (const RetiredConfigKey& k : kRetiredConfigKeys) {
        ASSERT_NE(k.family, nullptr) << k.path;
        EXPECT_EQ(k.family->card, "RKD") << k.path;
        EXPECT_EQ(k.family->date, "2026-10-14") << k.path;
        EXPECT_FALSE(k.family->what.empty()) << k.path;
        const std::string commit(k.family->commit);
        if (seen.empty() || seen.back().first != commit) {
            seen.emplace_back(commit, 0);
        } else {
            EXPECT_LT(previous_path, k.path) << "rows of family " << commit << " out of order";
        }
        ++seen.back().second;
        previous_path = k.path;
    }
    EXPECT_EQ(seen, kKeysPerCommit);
}

TEST(RetiredConfigKeys, EveryJvalKeyHasItsOwnFlatSpelling) {
    // The old loader read 149 of the keys through jval with a flat legacy key; the station
    // member was read from each roster object and had none.
    std::set<std::string> flats;
    for (const RetiredConfigKey& k : kRetiredConfigKeys) {
        const bool array_member = k.path.find("[]") != std::string_view::npos;
        EXPECT_EQ(k.flat.empty(), array_member) << k.path;
        if (k.flat.empty()) continue;
        EXPECT_EQ(k.flat.find('.'), std::string_view::npos) << k.flat;
        EXPECT_TRUE(flats.insert(std::string(k.flat)).second) << "flat spelling shared: " << k.flat;
    }
    EXPECT_EQ(flats.size(), 149u);
}

TEST(RetiredConfigKeys, NoRowNamesAKeyTheCurrentLedgerLists) {
    // The ledger holds one row per key the network loader reads: a retired key with a row
    // would warn about a live key.
    const nlohmann::json ledger =
        parseFile(fs::path(GNSS_PROJECT_ROOT) / "docs" / "CONFIG_promotion_ledger.json");
    std::set<std::string> live;
    for (const auto& r : ledger.at("knobs")) {
        const std::string knob = r.at("knob").get<std::string>();
        live.insert(knob.substr(0, knob.find(' ')));
    }
    ASSERT_GT(live.size(), 500u) << "the ledger was not read";
    for (const RetiredConfigKey& k : kRetiredConfigKeys)
        EXPECT_EQ(live.count(std::string(k.path)), 0u) << k.path << " has a live ledger row";
}

TEST(RetiredConfigKeys, CurrentGoldensSetNoneAndTheRetiredFieldGoldenSetsItsLanesAndGates) {
    const fs::path dir = fs::path(GNSS_PROJECT_ROOT) / "config" / "golden";
    for (const char* name : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        const auto hits = findRetiredConfigKeys(parseFile(dir / name));
        EXPECT_TRUE(hits.empty()) << name << " sets " << joined(pathsOf(hits));
    }
    // Retired and frozen: it armed the lanes, the SH gate, the deweight gate and the PB filter.
    const auto old = pathsOf(findRetiredConfigKeys(parseFile(dir / "golden_field_gps_dual.json")));
    EXPECT_EQ(old.size(), 28u) << joined(old);
    for (const char* k : {"network.mw_wl.ewl_enable", "network.sh_health_gate",
                          "network.iono_constraint_deweight_adaptive", "network.emit_pb_filter"})
        EXPECT_NE(std::find(old.begin(), old.end(), k), old.end()) << k;
}

// ---------------------------------------------------------------------------
//  2. The scan reads what the old loader read
// ---------------------------------------------------------------------------

TEST(RetiredConfigKeys, FindsEitherSpellingOnceWithTheValueTheOldLoaderRead) {
    const auto dotted = findRetiredConfigKeys(
        nlohmann::json::parse(R"({"network": {"mw_wl": {"enable": true}}})"));
    ASSERT_EQ(dotted.size(), 1u);
    EXPECT_EQ(dotted[0].key->path, "network.mw_wl.enable");
    EXPECT_EQ(dotted[0].spellings, std::vector<std::string>{"network.mw_wl.enable"});
    EXPECT_EQ(dotted[0].value, "true");
    EXPECT_EQ(dotted[0].entries, 0u);

    const auto flat = findRetiredConfigKeys(nlohmann::json::parse(R"({"network_mw_wl_enable": false})"));
    ASSERT_EQ(flat.size(), 1u);
    EXPECT_EQ(flat[0].key->path, "network.mw_wl.enable");
    EXPECT_EQ(flat[0].spellings, std::vector<std::string>{"network_mw_wl_enable"});
    EXPECT_EQ(flat[0].value, "false");

    // Both spellings: one hit, and the value is the dotted path's, which jval tried first.
    const auto both = findRetiredConfigKeys(nlohmann::json::parse(
        R"({"network": {"mw_wl": {"enable": true}}, "network_mw_wl_enable": false})"));
    ASSERT_EQ(both.size(), 1u);
    EXPECT_EQ(both[0].spellings,
              (std::vector<std::string>{"network.mw_wl.enable", "network_mw_wl_enable"}));
    EXPECT_EQ(both[0].value, "true");

    // Several keys come back in table order, whatever their order in the document.
    const auto several = findRetiredConfigKeys(nlohmann::json::parse(
        R"({"parallel_reduce": true, "network": {"emit_pb_filter": true, "serialize_sbasis_contract": true}})"));
    EXPECT_EQ(pathsOf(several), (std::vector<std::string>{"network.serialize_sbasis_contract",
                                                          "network.emit_pb_filter",
                                                          "network.parallel_reduce"}));
}

TEST(RetiredConfigKeys, StationMemberIsOneHitForTheWholeRoster) {
    const auto hits = findRetiredConfigKeys(nlohmann::json::parse(R"({"network": {"stations": [
        {"name": "AAAA", "fixed": true}, {"name": "BBBB"}, {"name": "CCCC", "fixed": false}]}})"));
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].key->path, "network.stations[].fixed");
    EXPECT_EQ(hits[0].entries, 2u);
    EXPECT_EQ(hits[0].value, "true");
    EXPECT_EQ(hits[0].spellings, std::vector<std::string>{"network.stations[].fixed"});
}

TEST(RetiredConfigKeys, LiveKeysNearMissesAndUnreadSpellingsAreSilent) {
    const nlohmann::json cfg = nlohmann::json::parse(R"({
        "network": {
            "raim": true,
            "parallel_carry": true,
            "clock_level_monitor": true,
            "cycle_slip": {"mw_reflag_skip": true},
            "mw_wl": {},
            "fast_clock": {"enabled": true},
            "stations": [{"name": "AAAA", "position_ecef": [1.0, 2.0, 3.0]}],
            "network_mw_wl_enable": true
        },
        "baseline": {"reference": {"fixed": true}},
        "network.emit_pb_filter": true,
        "mw_wl_enable": true
    })");
    // network.network_mw_wl_enable: jval tried the flat key at the top level only.
    // "network.emit_pb_filter" as one literal key: resolvePath walks nested objects.
    // fast_clock.enabled and mw_wl_enable: near misses.  baseline.reference.fixed: live.
    const auto hits = findRetiredConfigKeys(cfg);
    EXPECT_TRUE(hits.empty()) << joined(pathsOf(hits));
    EXPECT_TRUE(findRetiredConfigKeys(nlohmann::json::array()).empty());
}

TEST(RetiredConfigKeys, MessageNamesTheKeyItsSpellingsAndTheDeletion) {
    const auto dotted = findRetiredConfigKeys(nlohmann::json::parse(R"({"network": {"emit_pb_filter": true}})"));
    ASSERT_EQ(dotted.size(), 1u);
    EXPECT_EQ(retiredConfigKeyMessage(dotted[0]),
              "Config: retired key network.emit_pb_filter (value true) is ignored. Card RKD deleted it "
              "on 2026-10-14 with the PB emission-continuity filter (commit 6d719e4); the key has no "
              "effect.");

    const auto flat = findRetiredConfigKeys(nlohmann::json::parse(R"({"carry_iono": true})"));
    ASSERT_EQ(flat.size(), 1u);
    EXPECT_EQ(retiredConfigKeyMessage(flat[0]),
              "Config: retired key network.carry_iono (spelled carry_iono, value true) is ignored. Card "
              "RKD deleted it on 2026-10-14 with the diagonal slant-iono carry (commit f4be1a8); the key "
              "has no effect.");

    const auto both = findRetiredConfigKeys(nlohmann::json::parse(
        R"({"network": {"fast_clock": {"enable": true}}, "network_fast_clock_enable": false})"));
    ASSERT_EQ(both.size(), 1u);
    EXPECT_NE(retiredConfigKeyMessage(both[0]).find(
                  "(spelled network.fast_clock.enable and network_fast_clock_enable, value true)"),
              std::string::npos)
        << retiredConfigKeyMessage(both[0]);

    const auto roster = findRetiredConfigKeys(nlohmann::json::parse(
        R"({"network": {"stations": [{"name": "A", "fixed": false}, {"name": "B", "fixed": true}]}})"));
    ASSERT_EQ(roster.size(), 1u);
    EXPECT_NE(retiredConfigKeyMessage(roster[0]).find(
                  "retired key network.stations[].fixed (set in 2 entries of network.stations, "
                  "first value false) is ignored"),
              std::string::npos)
        << retiredConfigKeyMessage(roster[0]);

    // A long value is cut to 60 characters and marked.
    const auto pairs = findRetiredConfigKeys(nlohmann::json::parse(R"({"network": {"mw_wl":
        {"uwl_family_pairs": [["G", "L1C", "L2W"], ["G", "L2W", "L5Q"], ["E", "L1C", "L5Q"], ["E", "L5Q", "L7Q"]]}}})"));
    ASSERT_EQ(pairs.size(), 1u);
    EXPECT_EQ(pairs[0].value.size(), 60u);
    EXPECT_EQ(pairs[0].value.substr(57), "...");
}

// ---------------------------------------------------------------------------
//  3. The loader warns and does nothing else
// ---------------------------------------------------------------------------

TEST(RetiredConfigKeys, LoaderWarnsOncePerKeyAndLogsNothingElseNew) {
    const nlohmann::json base = nlohmann::json::parse(TestSupport::networkConfig(""));
    nlohmann::json with = base;
    with["network"]["mw_wl"]["ewl_enable"] = true;           // dotted
    with["network_mw_wl_ewl_enable"] = false;                 // and flat: still one warning
    with["network"]["emit_pb_filter"] = true;                 // dotted
    with["sh_health_gate"] = true;                            // flat only
    with["network"]["fast_clock"] = {{"enable", true}, {"trace", true}};   // two keys, one object
    for (auto& station : with["network"]["stations"]) station["fixed"] = true;   // the roster member

    bool ok_base = false;
    bool ok_with = false;
    const std::vector<std::string> base_log = loadAndLog(base, ok_base);
    const std::vector<std::string> with_log = loadAndLog(with, ok_with);
    ASSERT_TRUE(ok_base);
    ASSERT_TRUE(ok_with);

    std::vector<std::string> warnings;
    std::vector<std::string> rest;
    for (const std::string& line : with_log)
        (isRetiredKeyWarning(line) ? warnings : rest).push_back(line);
    for (const std::string& line : base_log) EXPECT_FALSE(isRetiredKeyWarning(line)) << line;

    // One warning per key, in table order, and the rest of the log line for line as without them.
    ASSERT_EQ(warnings.size(), 6u) << joined(warnings);
    const char* const keys[] = {"network.stations[].fixed", "network.mw_wl.ewl_enable",
                                "network.fast_clock.enable", "network.fast_clock.trace",
                                "network.emit_pb_filter", "network.sh_health_gate"};
    for (std::size_t i = 0; i < warnings.size(); ++i)
        EXPECT_EQ(warnings[i].rfind(std::string("warning Config: retired key ") + keys[i] + " (", 0), 0u)
            << warnings[i];
    EXPECT_EQ(rest, base_log);
}
