/**
 * @file test_network_knobs_epoch_driver.cpp
 * @brief The epoch driver's configuration-derived values in NetworkKnobs (card NOM3 step
 *        EP0) equal the expressions runNetwork computed before they moved.
 *
 * Card NOM3 step EP0 moved the values runNetwork derived from the configuration, once in
 * its setup or again on every epoch (the section timers, the resolved clock anchor sigma,
 * the SH-deviation switches, the VCE monitor's families, the Galileo top band, the
 * streaming source, the within-epoch tier, the phase-datum switches, the max-duration and
 * decimation limits, the constant half of the RHS attribution window, the trace switches,
 * the configuration half of the leveling warm-up and the float vehicle's birth-floor
 * arm), into NetworkKnobs::snapshot (the Galileo top band has since gone with the UWL lane
 * tier).  The expressions in driverValues() are the driver's, copied from
 * ApplicationNetwork.cpp as it stood before the step (main 8326da3 with steps HV0 and
 * WCF), with one substitution: where the driver tested the SH model (setup.sh_model), the
 * copy tests network.estimate_iono_sh, under which setup builds it.  The election's
 * ledger read is checked too: the driver's two conjunctions, with ctx.roster.active true
 * as it is on every network run, against the existing knobs it now reads.  The gap-resume
 * census arm is not a knob: the census reads the slip-QC parameters' own predicate
 * (SlipQcParams::collectsResumptions), which its case checks against the driver's former
 * expression.
 *
 * Every posture checks each knob against its expression; each armed posture also checks
 * the value it targets (a switch on, a band digit), so a posture the loader quietly
 * disarms cannot pass by reading the default on both sides.
 */

#include <gtest/gtest.h>

#include "network/state/RunContext.hpp"
#include "config/GnssConfig.hpp"
#include "network/carry/CarryVce.hpp"
#include "logging/LogManager.hpp"
#include "network/slip/RawObservables.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace GnssProcessor;

namespace {

/// A minimal two-station GPS network configuration: @p processing_extra in its processing
/// section, @p network_extra in its network section and @p root_extra (whole sections, each
/// followed by a comma) at the top level.
std::string networkConfig(const std::string& network_extra,
                          const std::string& processing_extra = "",
                          const std::string& root_extra = "") {
    return std::string("{\n") + root_extra +
        "  \"processing\": {\n" + processing_extra +
        "    \"mode\": \"ppp-rtk-network\",\n"
        "    \"active_systems\": [ \"GPS\" ],\n"
        "    \"signal_priorities\": { \"GPS\": \"WC\" }\n"
        "  },\n"
        "  \"network\": {\n"
        "    \"clock_datum_station\": \"AAAA\",\n"
        + network_extra +
        "    \"stations\": [\n"
        "      { \"name\": \"AAAA\", \"position_ecef\": [ -2493304.99, -4655214.79, 3565497.67 ] },\n"
        "      { \"name\": \"BBBB\", \"position_ecef\": [ -2281621.75, -1453585.08, 5756964.89 ] }\n"
        "    ]\n"
        "  }\n"
        "}\n";
}

/// Write @p body to a temporary file and load it into @p cfg.
bool loadInto(const std::string& tag, const std::string& body, GnssConfig& cfg) {
    LogManager log;
    const fs::path p = fs::temp_directory_path() / ("knobs_epoch_driver_cfg_" + tag + ".json");
    {
        std::ofstream f(p);
        f << body;
    }
    const bool ok = cfg.loadFromJson(p, log);
    std::error_code ec;
    fs::remove(p, ec);
    return ok;
}

/// Load a golden configuration (config/golden/@p name) into @p cfg.
bool loadGolden(const char* name, GnssConfig& cfg) {
    LogManager log;
    return cfg.loadFromJson(fs::path(GNSS_PROJECT_ROOT) / "config" / "golden" / name, log);
}

/// The values as the driver computed them before card NOM3 step EP0.
struct DriverValues {
    bool sec_timers_on;
    double clk_anchor_sigma;
    double sh_dev_tau;
    bool sh_dev_on, sh_dev_demean, sh_dev_d5_keep;
    bool vce_wants_pb, vce_wants_cb;
    bool streaming_on, within_epoch_armed;
    bool sticky, anc_slip_handover, gate_drop_erase;
    int min_stations;
    double max_duration;
    std::size_t dn;
    bool rhs_on_trip;
    double rhs_tow;
    int rhs_n_ep;
    bool trace_postfit_on, trace_sh_spectrum, trace_station_block;
    bool birth_floor_on;
    bool reentry_observe, reentry_restore;
};

DriverValues driverValues(const GnssConfig& config_, const NetworkConfig& nc) {
    // Setup builds the SH model exactly when network.estimate_iono_sh is set.
    const bool sh_model = config_.estimateNetworkIonoSh();
    const GlobalVtecShConfig& shc = config_.getGlobalVtecShConfig();
    const bool roster_active = true;   // networkSignalRosterArm, every network run
    DriverValues d{};
    d.sec_timers_on = nc.trace_section_timers;
    d.clk_anchor_sigma = config_.resolveNetworkAprioriClockAnchorSigmaM();
    d.sh_dev_tau = sh_model ? shc.rep_error_tau_s : 0.0;
    d.sh_dev_on  = d.sh_dev_tau > 0.0;
    d.sh_dev_demean = d.sh_dev_on && shc.rep_error_demean;
    d.sh_dev_d5_keep = d.sh_dev_on && shc.rep_error_d5_keep;
    const bool carry_vce = config_.networkCarryVce();
    const std::vector<std::string> vce_families =
        carry_vce ? NetworkCarryVce::parseComponents(config_.networkCarryVceComponents())
                  : std::vector<std::string>{};
    d.vce_wants_pb = std::find(vce_families.begin(), vce_families.end(),
                               std::string("pb")) != vce_families.end();
    d.vce_wants_cb = std::find(vce_families.begin(), vce_families.end(),
                               std::string("cb")) != vce_families.end();
    d.streaming_on = nc.streaming.enable;
    d.within_epoch_armed = config_.networkRaimWithinEpoch();
    d.sticky = config_.networkPhaseDatumSticky();
    d.anc_slip_handover = config_.networkCycleSlipAnchorSlipHandover();
    d.gate_drop_erase = config_.networkPhaseDatumGateDropErase();
    d.min_stations = config_.networkPhaseDatumMinStations();
    d.max_duration = config_.getMaxDuration();
    d.dn = static_cast<size_t>(std::max(1, nc.epoch_decimation_n));
    d.rhs_on_trip = nc.rhs_attrib_on_trip;
    d.rhs_tow = nc.rhs_attrib_tow;
    d.rhs_n_ep = std::max(1, nc.rhs_attrib_n_ep);
    d.trace_postfit_on = nc.trace_postfit_sigma0 || nc.trace_postfit_keyed || nc.trace_postfit_rows;
    d.trace_sh_spectrum = nc.trace_sh_spectrum;
    d.trace_station_block = nc.trace_station_block;
    d.birth_floor_on = (config_.getNetworkConfig().ambiguity_vehicle == NetworkAmbiguityVehicle::FloatKf)
                       && config_.getNetworkConfig().amb_float_birth_var_m2 > 0.0;
    d.reentry_observe = config_.networkLedgerScreens() && roster_active;
    d.reentry_restore = d.reentry_observe && config_.networkLedgerAnchor();
    return d;
}

/// Every knob of the step equals its driver expression; returns the snapshot.
NetworkKnobs expectDriverValues(const GnssConfig& cfg, const NetworkConfig& nc) {
    const NetworkKnobs k = NetworkKnobs::snapshot(cfg, nc);
    const DriverValues d = driverValues(cfg, nc);
    EXPECT_EQ(k.sec_timers_on, d.sec_timers_on);
    EXPECT_EQ(k.clk_anchor_sigma, d.clk_anchor_sigma);
    EXPECT_EQ(k.sh_dev_tau, d.sh_dev_tau);
    EXPECT_EQ(k.sh_dev_on, d.sh_dev_on);
    EXPECT_EQ(k.sh_dev_demean, d.sh_dev_demean);
    EXPECT_EQ(k.sh_dev_d5_keep, d.sh_dev_d5_keep);
    EXPECT_EQ(k.vce_wants_pb, d.vce_wants_pb);
    EXPECT_EQ(k.vce_wants_cb, d.vce_wants_cb);
    EXPECT_EQ(k.streaming_on, d.streaming_on);
    EXPECT_EQ(k.within_epoch_armed, d.within_epoch_armed);
    EXPECT_EQ(k.pd_sticky, d.sticky);
    EXPECT_EQ(k.anc_slip_handover, d.anc_slip_handover);
    EXPECT_EQ(k.pd_gate_drop_erase, d.gate_drop_erase);
    EXPECT_EQ(k.pd_min_stations, d.min_stations);
    EXPECT_EQ(k.max_duration_s, d.max_duration);
    EXPECT_EQ(k.epoch_decimation_n, d.dn);
    EXPECT_EQ(k.rec_on, d.rhs_on_trip);
    EXPECT_EQ(k.rhs_attrib_tow, d.rhs_tow);
    EXPECT_EQ(k.rhs_attrib_n_ep, d.rhs_n_ep);
    EXPECT_EQ(k.trace_postfit_on, d.trace_postfit_on);
    EXPECT_EQ(k.trace_sh_spectrum, d.trace_sh_spectrum);
    EXPECT_EQ(k.trace_station_block, d.trace_station_block);
    EXPECT_EQ(k.amb_birth_floor_on, d.birth_floor_on);
    // The election's ledger read (AnchorReentry::observe / restore) as the driver now forms
    // it, with ctx.roster.active true as on every network run.
    const bool roster_active = true;
    const bool observe = k.ledger_screens && roster_active;
    EXPECT_EQ(observe, d.reentry_observe);
    EXPECT_EQ(observe && k.ledger_anchor, d.reentry_restore);
    return k;
}

NetworkKnobs expectDriverValues(const GnssConfig& cfg) {
    return expectDriverValues(cfg, cfg.getNetworkConfig());
}

}  // namespace

TEST(NetworkKnobsEpochDriver, DefaultsMatchTheDriver) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("default", networkConfig(""), cfg));
    const NetworkKnobs k = expectDriverValues(cfg);
    EXPECT_FALSE(k.sec_timers_on);
    EXPECT_FALSE(k.sh_dev_on);
    EXPECT_EQ(k.sh_dev_tau, 0.0);
    EXPECT_FALSE(k.vce_wants_pb);
    EXPECT_FALSE(k.streaming_on);
    EXPECT_FALSE(k.within_epoch_armed);
    EXPECT_EQ(k.epoch_decimation_n, 1u);
    EXPECT_LT(k.rhs_attrib_tow, 0.0);
    EXPECT_FALSE(k.trace_postfit_on);
    EXPECT_FALSE(k.amb_birth_floor_on);
}

// The values the driver read from its NetworkConfig, set both ways on a copy of the
// configuration's block and read from the same block the snapshot is given.
TEST(NetworkKnobsEpochDriver, NetworkBlockValuesBothWays) {
    GnssConfig cfg;
    ASSERT_TRUE(loadInto("block", networkConfig(""), cfg));
    NetworkConfig nc = cfg.getNetworkConfig();
    nc.trace_section_timers = true;
    nc.streaming.enable = true;
    nc.epoch_decimation_n = 3;
    nc.rhs_attrib_on_trip = true;
    nc.rhs_attrib_tow = 345600.0;
    nc.rhs_attrib_n_ep = 5;
    nc.trace_postfit_rows = true;
    nc.trace_sh_spectrum = true;
    nc.trace_station_block = true;
    NetworkKnobs k = expectDriverValues(cfg, nc);
    EXPECT_TRUE(k.sec_timers_on);
    EXPECT_TRUE(k.streaming_on);
    EXPECT_EQ(k.epoch_decimation_n, 3u);
    EXPECT_TRUE(k.rec_on);
    EXPECT_EQ(k.rhs_attrib_tow, 345600.0);
    EXPECT_EQ(k.rhs_attrib_n_ep, 5);
    EXPECT_TRUE(k.trace_postfit_on);
    EXPECT_TRUE(k.trace_sh_spectrum);
    EXPECT_TRUE(k.trace_station_block);

    // The other half of each disjunction, and the clamps.
    nc.epoch_decimation_n = 0;
    nc.rhs_attrib_n_ep = 0;
    nc.trace_postfit_rows = false;
    nc.trace_postfit_keyed = true;
    k = expectDriverValues(cfg, nc);
    EXPECT_EQ(k.epoch_decimation_n, 1u);
    EXPECT_EQ(k.rhs_attrib_n_ep, 1);
    EXPECT_TRUE(k.trace_postfit_on);
    nc.trace_postfit_keyed = false;
    nc.trace_postfit_sigma0 = true;
    k = expectDriverValues(cfg, nc);
    EXPECT_TRUE(k.trace_postfit_on);
}

TEST(NetworkKnobsEpochDriver, ConfigurationValuesArmed) {
    GnssConfig cfg;
    const std::string network =
        "    \"raim\": true,\n"
        "    \"raim_within_epoch\": true,\n"
        "    \"apriori_clock_anchor_sigma_m\": 2.5,\n"
        "    \"phase_datum_prioritized\": true,\n"
        "    \"phase_datum_sticky\": false,\n"
        "    \"phase_datum_min_stations\": 3,\n"
        "    \"phase_datum_gate_drop_erase\": true,\n"
        "    \"cycle_slip\": { \"anchor_slip_handover\": true },\n"
        "    \"ledger_screens\": true,\n"
        "    \"ledger_anchor\": true,\n";
    ASSERT_TRUE(loadInto("armed", networkConfig(network, "    \"max_duration_s\": 7200.0,\n"),
                         cfg));
    const NetworkKnobs k = expectDriverValues(cfg);
    EXPECT_TRUE(k.within_epoch_armed);
    EXPECT_EQ(k.clk_anchor_sigma, 2.5);
    EXPECT_FALSE(k.pd_sticky);
    EXPECT_EQ(k.pd_min_stations, 3);
    EXPECT_TRUE(k.pd_gate_drop_erase);
    EXPECT_TRUE(k.anc_slip_handover);
    EXPECT_EQ(k.max_duration_s, 7200.0);
    EXPECT_TRUE(k.ledger_screens);
    EXPECT_TRUE(k.ledger_anchor);
}

TEST(NetworkKnobsEpochDriver, CarryVceFamilies) {
    GnssConfig cfg;
    const std::string network =
        "    \"phase_leveling\": true,\n"
        "    \"cycle_slip\": { \"mw_detect\": true },\n"
        "    \"carry_vce\": true,\n"
        "    \"carry_vce_components\": \"pl, pb\",\n";
    ASSERT_TRUE(loadInto("vce", networkConfig(network), cfg));
    ASSERT_TRUE(cfg.networkCarryVce());
    NetworkKnobs k = expectDriverValues(cfg);
    EXPECT_TRUE(k.vce_wants_pb);
    EXPECT_FALSE(k.vce_wants_cb);

    GnssConfig cfg2;
    const std::string network2 =
        "    \"phase_leveling\": true,\n"
        "    \"cycle_slip\": { \"mw_detect\": true },\n"
        "    \"carry_vce\": true,\n"
        "    \"carry_vce_components\": \"cb;pl\",\n";
    ASSERT_TRUE(loadInto("vce2", networkConfig(network2), cfg2));
    k = expectDriverValues(cfg2);
    EXPECT_FALSE(k.vce_wants_pb);
    EXPECT_TRUE(k.vce_wants_cb);
}

// The SH-deviation switches follow the SH field: the same estimation block reads
// 0 / off without network.estimate_iono_sh.
TEST(NetworkKnobsEpochDriver, ShDeviationFollowsTheShField) {
    const std::string estimation =
        "  \"estimation\": {\n"
        "    \"global_vtec_sh\": {\n"
        "      \"rep_error_tau_s\": 900.0,\n"
        "      \"rep_error_demean\": true,\n"
        "      \"rep_error_d5_keep\": true\n"
        "    }\n"
        "  },\n";
    const std::string leveling =
        "    \"phase_leveling\": true,\n"
        "    \"cycle_slip\": { \"mw_detect\": true },\n";
    GnssConfig field;
    ASSERT_TRUE(loadInto("sh_on",
                         networkConfig(leveling + "    \"estimate_iono_sh\": true,\n", "",
                                       estimation),
                         field));
    ASSERT_TRUE(field.estimateNetworkIonoSh());
    NetworkKnobs k = expectDriverValues(field);
    EXPECT_EQ(k.sh_dev_tau, 900.0);
    EXPECT_TRUE(k.sh_dev_on);
    EXPECT_TRUE(k.sh_dev_demean);
    EXPECT_TRUE(k.sh_dev_d5_keep);

    GnssConfig no_field;
    ASSERT_TRUE(loadInto("sh_off", networkConfig(leveling, "", estimation), no_field));
    ASSERT_FALSE(no_field.estimateNetworkIonoSh());
    k = expectDriverValues(no_field);
    EXPECT_EQ(k.sh_dev_tau, 0.0);
    EXPECT_FALSE(k.sh_dev_on);
    EXPECT_FALSE(k.sh_dev_demean);
    EXPECT_FALSE(k.sh_dev_d5_keep);
}

// The float vehicle's birth-floor arm, both conjuncts: on with float_kf and a positive
// floor, off with the same floor on the level vehicle (the goldens run float_kf with no
// floor, so they cover only the floor's absence).
TEST(NetworkKnobsEpochDriver, BirthFloorNeedsTheFloatVehicle) {
    const std::string vehicle =
        "    \"phase_leveling\": true,\n"
        "    \"carry_invariant\": false,\n"
        "    \"carry_conditional_coupling\": true,\n"
        "    \"shared_carry_full_cov\": true,\n"
        "    \"cycle_slip\": { \"mw_detect\": true },\n"
        "    \"amb_float_birth_var_m2\": 0.25,\n";

    GnssConfig fkf;
    ASSERT_TRUE(loadInto("birth_fkf",
                         networkConfig(vehicle + "    \"ambiguity_vehicle\": \"float_kf\",\n"),
                         fkf));
    NetworkKnobs k = expectDriverValues(fkf);
    EXPECT_TRUE(k.amb_float_kf);
    EXPECT_EQ(k.amb_birth_var_floor, 0.25);
    EXPECT_TRUE(k.amb_birth_floor_on);

    GnssConfig level;
    ASSERT_TRUE(loadInto("birth_level", networkConfig(vehicle), level));
    k = expectDriverValues(level);
    EXPECT_FALSE(k.amb_float_kf);
    EXPECT_EQ(k.amb_birth_var_floor, 0.25);
    EXPECT_FALSE(k.amb_birth_floor_on);
}

// The arc-resumption bridge census is armed by the slip-QC parameters' own predicate, which
// the driver copies from network.cycle_slip.gap_instrument and gap_reentry_relevel once at
// run start: its truth table is the driver's former expression, and it is off by default.
TEST(NetworkKnobsEpochDriver, GapCensusArmIsTheSlipQcPredicate) {
    EXPECT_FALSE(SlipQcParams{}.collectsResumptions());
    for (const bool instrument : {false, true}) {
        for (const bool reentry : {false, true}) {
            SlipQcParams p;
            p.gap_instrument = instrument;
            p.gap_reentry    = reentry;
            EXPECT_EQ(p.collectsResumptions(), instrument || reentry)
                << "gap_instrument " << instrument << ", gap_reentry " << reentry;
        }
    }
}

TEST(NetworkKnobsEpochDriver, BothGoldens) {
    for (const char* name : {"golden_field_refound.json", "golden_sim_refound.json"}) {
        GnssConfig cfg;
        ASSERT_TRUE(loadGolden(name, cfg)) << name;
        const NetworkKnobs k = expectDriverValues(cfg);
        // Both goldens arm the within-epoch tier and run the float vehicle.
        EXPECT_TRUE(k.within_epoch_armed) << name;
        EXPECT_TRUE(k.amb_float_kf) << name;
        EXPECT_EQ(k.amb_birth_floor_on, k.amb_birth_var_floor > 0.0) << name;
    }
}

// The VCE monitor's hourly flush schedule moved from two driver locals into the run carry
// with the locals' initial values (step EP0, item A3).
TEST(NetworkKnobsEpochDriver, VceFlushScheduleStartsUnanchored) {
    const auto carry = std::make_unique<NetworkCarryState>();
    EXPECT_EQ(carry->last_vce_hour, -1);
    EXPECT_EQ(carry->first_vce_s, -1.0);
}

// Card NOM3 step SG: the retro-bump / URA-inflate feed, formerly a local of the solve
// (`ura_retro || ura_refloat_inflate > 1.0`), is a knob read by the within-epoch tier and the
// solve's result.
TEST(NetworkKnobsEpochDriver, RetroFeedIsEitherUraConsumer) {
    GnssConfig off;
    ASSERT_TRUE(loadInto("retro_off", networkConfig(""), off));
    const NetworkKnobs k_off = NetworkKnobs::snapshot(off, off.getNetworkConfig());
    EXPECT_FALSE(k_off.retro_feed);
    EXPECT_EQ(k_off.retro_feed, k_off.ura_retro || k_off.ura_refloat_inflate > 1.0);
    GnssConfig inflate;
    ASSERT_TRUE(loadInto("retro_inflate", networkConfig("    \"ura_refloat_inflate\": 2.0,\n"), inflate));
    const NetworkKnobs k = NetworkKnobs::snapshot(inflate, inflate.getNetworkConfig());
    EXPECT_EQ(k.retro_feed, k.ura_retro || k.ura_refloat_inflate > 1.0);
    EXPECT_TRUE(k.retro_feed);
}
