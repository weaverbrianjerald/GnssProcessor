/**
 * @file SlipOracle.cpp
 * @brief Implementation of the sim-only oracle slip response (the ceiling of a
 *        level-preserving re-level).  See the header banner for the card, the
 *        window semantics and why it is field-inert.
 */

#include "network/slip/SlipOracle.hpp"

#include "common/GnssIdentity.hpp"
#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

// The run-end reports at the end of this file:
#include "network/state/RunContext.hpp"
#include "network/runend/RunEnd.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include "network/driver/Run.hpp"
#include "network/driver/SetupResult.hpp"
#include "network/diag/RunTraces.hpp"

namespace GnssProcessor {
namespace {

/// Trim ASCII whitespace (and a stray CR from a Windows-authored fixture).
std::string trim(std::string s) {
    const auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
    return s;
}

/// splitmix64: a full-avalanche 64-bit mixer, so the seeds of two arcs that
/// differ in one character differ in about half their bits.
std::uint64_t mix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

std::uint64_t hashStr(std::uint64_t h, const std::string& s) {
    for (const char c : s) {          // FNV-1a body, mixed at the end
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 0x100000001B3ULL;
    }
    return h;
}

} // namespace

NetworkSlipOracle::~NetworkSlipOracle() = default;

// ===========================================================================
// CSV
// ===========================================================================

std::map<std::pair<std::string, std::string>,
         std::vector<NetworkSlipOracle::TruthSlip>>
networkSlipOracleParseCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error(
            "network.cycle_slip.oracle_relevel: cannot open truth slip file '" + path + "'");

    std::map<std::pair<std::string, std::string>,
             std::vector<NetworkSlipOracle::TruthSlip>> out;

    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error(
            "network.cycle_slip.oracle_relevel: truth slip file '" + path + "' is empty");
    // Header contract (src/sim/main.cpp):
    //   week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg
    // The first five column names are checked as a prefix of the header, so a
    // fixture that inserts or reorders a column among them fails here instead of
    // mis-reading delta_cycles.  Columns after delta_cycles are not checked.
    {
        const std::string hdr = trim(line);
        if (hdr.rfind("week,tow_s,satellite,signal,delta_cycles", 0) != 0)
            throw std::runtime_error(
                "network.cycle_slip.oracle_relevel: unexpected truth slip header in '" + path
                + "' (want 'week,tow_s,satellite,signal,delta_cycles,...', got '" + hdr + "')");
    }

    long long row = 1;
    while (std::getline(in, line)) {
        ++row;
        const std::string t = trim(line);
        if (t.empty()) continue;
        std::istringstream ss(t);
        std::string f_week, f_tow, f_sat, f_sig, f_del;
        if (!std::getline(ss, f_week, ',') || !std::getline(ss, f_tow, ',')
            || !std::getline(ss, f_sat, ',') || !std::getline(ss, f_sig, ',')
            || !std::getline(ss, f_del, ','))
            throw std::runtime_error(fmt::format(
                "network.cycle_slip.oracle_relevel: malformed truth slip row {} in '{}' "
                "(want >= 5 comma-separated fields)", row, path));
        try {
            const double week = std::stod(trim(f_week));
            const double tow  = std::stod(trim(f_tow));
            const int    del  = static_cast<int>(std::lround(std::stod(trim(f_del))));
            const std::string sat = trim(f_sat), sig = trim(f_sig);
            if (sat.empty() || sig.empty())
                throw std::runtime_error("empty satellite or signal");
            // Continuous-GPS seconds, the same frame the drains compute as
            // week*604800 + tow (network/slip/SlipQc.cpp's windup drain).
            out[{sat, sig}].push_back({week * 604800.0 + tow, del});
        } catch (const std::exception& e) {
            throw std::runtime_error(fmt::format(
                "network.cycle_slip.oracle_relevel: malformed truth slip row {} in '{}': {}",
                row, path, e.what()));
        }
    }
    // The fixture writes in time order, and the window scan depends on that
    // order, so it is enforced here.  A stable sort keeps equal-stamped rows (two
    // jumps on one arc in one epoch) in file order so their sum is reproducible.
    for (auto& [k, v] : out)
        std::stable_sort(v.begin(), v.end(),
                         [](const NetworkSlipOracle::TruthSlip& a,
                            const NetworkSlipOracle::TruthSlip& b) { return a.s < b.s; });
    return out;
}

// ===========================================================================
// Load
// ===========================================================================

std::unique_ptr<NetworkSlipOracle> networkSlipOracleLoad(
        const GnssConfig& config_,
        const std::vector<NetStation>& stations,
        LogManager& log_manager,
        const std::shared_ptr<spdlog::logger>& log) {
    const auto& sq = config_.getNetworkConfig().slip_qc;

    auto oracle = std::make_unique<NetworkSlipOracle>();
    oracle->truth_root  = sq.oracle_truth_root;
    oracle->noise_cyc   = sq.oracle_noise_cyc;
    oracle->false_alarm = (sq.oracle_false_alarm == "free")
                              ? NetworkSlipOracle::FalseAlarm::Free
                              : NetworkSlipOracle::FalseAlarm::Keep;
    oracle->true_slip   = (sq.oracle_true_slip == "free")
                              ? NetworkSlipOracle::TrueSlip::Free
                              : NetworkSlipOracle::TrueSlip::Repair;
    // The loader already clamped this to >= 0; it is clamped again because this
    // function is also the unit suite's entry point (a hand-built config never
    // passes through loadFromJson).  A negative window, like 0, admits no truth
    // slip, so every actuation would read as a false alarm.
    oracle->window_s    = std::max(0.0, sq.oracle_window_s);
    oracle->utils       = std::make_unique<PppUtils>(log_manager);

    const std::filesystem::path root(oracle->truth_root);
    // The loader already hard-errored on a missing root; re-checked here because
    // this function is also the unit suite's entry point.
    if (oracle->truth_root.empty() || !std::filesystem::exists(root))
        throw std::runtime_error(
            "network.cycle_slip.oracle_relevel: oracle_truth_root '" + oracle->truth_root
            + "' does not exist.  This is a SIM-ONLY diagnostic knob — it is field-inert "
              "by construction and must never be armed on a field lineage.");

    long long n_rows = 0, n_arcs = 0;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
        const std::string& name = stations[static_cast<std::size_t>(i)].name;
        const auto file = root / name / "truth_slips.csv";
        if (!std::filesystem::exists(file))
            throw std::runtime_error(
                "network.cycle_slip.oracle_relevel: no truth slip file for ROSTER station '"
                + name + "' at '" + file.string()
                + "'.  A sim fixture writes one per station, so this is a mis-pointed "
                  "oracle_truth_root (or a roster that is not this fixture's), not a quiet "
                  "station — refusing to run a repair that would be silently partial.");
        const auto per_sta = networkSlipOracleParseCsv(file.string());
        for (const auto& [key, v] : per_sta) {
            oracle->truth[{i, key.first, key.second}] = v;
            n_rows += static_cast<long long>(v.size());
            ++n_arcs;
        }
    }

    log->warn("Network (slip ORACLE): network.cycle_slip.oracle_relevel is ON — a SIM-ONLY "
              "DIAGNOSTIC.  Every detector refloat on a CARRIED arc is answered from the "
              "truth table at '{}': {} truth slip(s) across {} (station, satellite, signal) "
              "arc(s) at {} roster station(s).  POLICY PAIR true_slip '{}' / false_alarm "
              "'{}'; injected noise sigma {:.4f} cyc; detector-latency window {:.0f} s, "
              "floored at each carry's own BIRTH second.  This measures the CEILING of a "
              "level-preserving re-level (SPEC_lane_slip_response) — it is NOT a shippable "
              "response and must never be promoted or land in a golden.",
              oracle->truth_root, n_rows, n_arcs, stations.size(),
              oracle->true_slip == NetworkSlipOracle::TrueSlip::Free ? "free" : "repair",
              oracle->false_alarm == NetworkSlipOracle::FalseAlarm::Free ? "free" : "keep",
              oracle->noise_cyc, oracle->window_s);
    return oracle;
}

// ===========================================================================
// Noise
// ===========================================================================

double networkSlipOracleNoise(double sigma_cyc, int sta, const std::string& sat,
                              const std::string& signal, double now_s) {
    if (!(sigma_cyc > 0.0)) return 0.0;
    // Deterministic seed: the arc identity plus the epoch, so a key drawn twice
    // (two detectors firing on one arc in one epoch, or a re-run of the whole
    // arm) gets the same draw.  The epoch is rounded to the millisecond so
    // floating-point noise in the tow cannot change it.
    std::uint64_t h = 0xCBF29CE484222325ULL;
    h = hashStr(h, sat);
    h = hashStr(h, signal);
    h ^= mix64(static_cast<std::uint64_t>(static_cast<std::int64_t>(sta)) + 0x1234567ULL);
    h ^= mix64(static_cast<std::uint64_t>(std::llround(now_s * 1000.0)));
    std::mt19937_64 gen(mix64(h));
    // Freshly constructed every call: std::normal_distribution caches a second
    // variate internally, so a shared instance would not be key-deterministic.
    std::normal_distribution<double> nd(0.0, sigma_cyc);
    return nd(gen);
}

// ===========================================================================
// The hook
// ===========================================================================

bool networkSlipOracleHandle(NetworkSlipOracle& oracle, int sta,
                             const std::string& sat, const std::string& signal,
                             double now_s, std::uint8_t cause,
                             std::string& detail, OracleOutcome& outcome) {
    outcome = OracleOutcome::NotHandled;
    ++oracle.seen;
    const auto key = std::make_tuple(sta, sat, signal);

    // (1) Nothing to preserve.  The card concerns the carried level; an arc with
    //     no leveling entry is re-estimated free either way, so the ordinary
    //     response is already the right one.
    if (!oracle.carry) { ++oracle.no_carry; return false; }
    const auto cit = net::getArc(*oracle.carry, key, &net::ArcState::amb);
    if (cit == nullptr) { ++oracle.no_carry; return false; }

    // (2) The window: (max(last_applied, carry.birth_s, now_s - window_s), now_s].
    //     See the header banner for why each term is there.  In particular the
    //     lower edge is not carry.last_seen_s: that is the previous epoch's stash
    //     time, and the intervening solve does not re-set a zero-q carried level,
    //     so a detector firing 1-3 epochs late (the normal latency) still sees an
    //     unabsorbed jump.
    auto& arc = oracle.arcs[key];
    const double w_edge = now_s - oracle.window_s;
    // Non-positive when the birth is unknown: 0 = never stamped,
    // LocalCarry::kBirthSRestored (−1) = warm-resume-restored (the true birth is
    // not snapshotted, SPEC_event_harness §15.4).  Either way it loses the max and
    // the window falls back to the latency bound.  The sentinel is kept across
    // re-stashes, so the arc is never floored at the resume epoch by a later
    // harvest.
    const double b_edge = cit->birth_s;
    const double lower  = std::max({arc.last_applied_s, b_edge, w_edge});
    // Which term bound (invariant #4).  Ties go to the most specific explanation
    // (applied, then birth, then window), so a reader can tell "the window is too
    // narrow" from "the arc is younger than the window" from "this jump was
    // already applied", which call for three different fixes.
    if (lower == arc.last_applied_s)      ++oracle.bound_applied;
    else if (lower == b_edge)             ++oracle.bound_birth;
    else                                  ++oracle.bound_window;
    long long n = 0;
    double    sum_cyc = 0.0;
    const auto tit = oracle.truth.find(key);
    if (tit != oracle.truth.end()) {
        for (const auto& ts : tit->second) {
            if (ts.s <= lower) continue;
            if (ts.s > now_s) break;              // ascending: nothing later qualifies
            sum_cyc += static_cast<double>(ts.delta_cyc);
            ++n;
        }
    }

    // (3) False alarm: the detector fired and the truth table has no jump on this
    //     arc in the window.  `keep` is the ceiling (an oracle knows); `free`
    //     falls through so the arm keeps the ordinary false-alarm cost.
    if (n == 0) {
        if (oracle.false_alarm == NetworkSlipOracle::FalseAlarm::Free) {
            ++oracle.fa_freed;
            return false;
        }
        ++oracle.fa_kept;
        // Per-tier attribution of the keeps (invariant #4).  The decision is
        // already made; the tier only says which detector's thresholds a
        // false-alarm-isolation arm is read against.  An unrecognised bit is a
        // mis-wired call site, not a fifth detector, so it gets its own bucket
        // instead of being folded into one of the four.
        switch (cause) {
            case RelevelEpochState::kCauseMw:     ++oracle.fa_kept_mw;    break;
            case RelevelEpochState::kCauseLli:    ++oracle.fa_kept_lli;   break;
            case RelevelEpochState::kCauseIfTd:   ++oracle.fa_kept_iftd;  break;
            case RelevelEpochState::kCauseWindup: ++oracle.fa_kept_wusat; break;
            default:                              ++oracle.fa_kept_other; break;
        }
        detail  = "delta=0;n=0;noise=0";
        outcome = OracleOutcome::Kept;
        return true;
    }

    // (3b) True slip under policy `free` (the false-alarm-isolation arm).  The
    //      truth table says this arc jumped, so it gets the ordinary response:
    //      decline, and the caller frees and re-levels as it would on the base
    //      arm.  This test comes before the wavelength lookup because no shift is
    //      formed: a signal with no wavelength is the same decline here, not a
    //      `no_lambda` fall-through (counting it in both would break the
    //      closure).  `last_applied_s` does not advance: nothing was applied, and
    //      the next detector on this arc sees the same window.
    if (oracle.true_slip == NetworkSlipOracle::TrueSlip::Free) {
        ++oracle.true_slip_freed;
        return false;
    }

    // (4) Repair.  Wavelength of the signal's own band (delta_cycles is in that
    //     signal's cycles), FDMA-correct via the satellite id.  An unknown signal
    //     is counted in no_lambda and falls through; skipping it without a count
    //     would read as a false alarm and bias the census.
    const double lambda =
        oracle.utils ? oracle.utils->getWavelength(systemOfSat(sat), signal, sat) : 0.0;
    if (!(lambda > 0.0) || !std::isfinite(lambda)) { ++oracle.no_lambda; return false; }

    const double noise = networkSlipOracleNoise(oracle.noise_cyc, sta, sat, signal, now_s);
    // Only value_abs is shifted.  The variance is left unchanged on purpose: in
    // the ceiling a perfect repair costs no information, so the level keeps its
    // converged weight.  (An x_lin shift would be an exact solve no-op;
    // docs/SPEC_lane_slip_response.md §7.2.)
    cit->value_abs += (sum_cyc + noise) * lambda;
    arc.last_applied_s = now_s;

    ++oracle.applied;
    if (n >= 2) ++oracle.multi;
    oracle.sum_abs_cyc += std::abs(sum_cyc);
    oracle.max_abs_cyc  = std::max(oracle.max_abs_cyc, std::abs(sum_cyc));

    detail  = fmt::format("delta={};n={};noise={:.4f}", sum_cyc, n, noise);
    outcome = OracleOutcome::Repaired;
    return true;
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the sim-only oracle slip response.
int networkSlipOracleRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // ---- Sim-only oracle slip response (include/network/slip/SlipOracle.hpp) ----
    // The actuation count (applied, or fa_kept under true_slip 'free'; see below) is
    // the only way to tell a working oracle from dead wiring (invariant #4).  The
    // pre-registered P1 is applied > 1500 on a 6 h campaign arm; a run that reports 0
    // has not measured a ceiling, it has measured nothing, and the line says so in the
    // log.
    if (!ctx.carry.slip_oracle) return 0;
    const auto& o = *ctx.carry.slip_oracle;
    const bool ts_free = (o.true_slip == NetworkSlipOracle::TrueSlip::Free);
    const bool fa_free = (o.false_alarm == NetworkSlipOracle::FalseAlarm::Free);
    // The actuation counter depends on the policy pair.  With true_slip 'free'
    // nothing is ever repaired by construction, so keying inertness on `applied`
    // would fire the dead-wiring warning on a correctly working false-alarm
    // isolation arm; that arm's own actuation is the keep (fa_kept).
    const long long acted = ts_free ? o.fa_kept : o.applied;
    log->warn("Network (slip ORACLE): {} detector actuation(s) on carried arcs, {} "
              "repaired (sum |delta| {:.1f} cyc, max |delta| {:.1f}, {} multi-slip), {} "
              "true-slip freed, {} false-alarm kept [MW {} | LLI {} | IF-TD {} | "
              "whole-sat {} | other {}], {} false-alarm freed, {} fell through with no "
              "carry, {} with no wavelength; noise sigma {:.4f} cyc; POLICY true_slip "
              "{} / false_alarm {}; detector-latency window {:.0f} s (floored at each "
              "carry's BIRTH second, so a level erased and re-created by T4 is never "
              "re-shifted) — lower bound set by the window on {}, by the carry's birth "
              "on {}, by an earlier repair on {}; truth root '{}'.  SIM-ONLY DIAGNOSTIC "
              "— this run's products carry a truth-fed response and are NOT a shippable "
              "posture.{}",
              o.seen, o.applied, o.sum_abs_cyc, o.max_abs_cyc, o.multi,
              o.true_slip_freed, o.fa_kept, o.fa_kept_mw, o.fa_kept_lli,
              o.fa_kept_iftd, o.fa_kept_wusat, o.fa_kept_other,
              o.fa_freed, o.no_carry, o.no_lambda, o.noise_cyc,
              ts_free ? "free" : "repair", fa_free ? "free" : "keep",
              o.window_s, o.bound_window, o.bound_birth, o.bound_applied, o.truth_root,
              (acted != 0)
                  ? ""
              : (ts_free && fa_free)
                  ? "  ⚠ NO-OP ARM BY CONFIGURATION: both policies are 'free', so the "
                    "oracle declined every actuation and wrote nothing.  This is the "
                    "byte-identity control, not a measurement."
              : ts_free
                  ? "  ⚠ ARMED AND INERT: ZERO false alarms were kept — on this policy "
                    "pair (true_slip 'free') the KEEP is the arm's only actuation, so "
                    "this is dead wiring, not a quiet run.  Do not read its ZWD as a "
                    "falsification of the false-alarm channel."
                  : "  ⚠ ARMED AND INERT: ZERO arcs were repaired — this is dead "
                    "wiring, not a quiet run, and the arm measures NOTHING.  Do not "
                    "read its ZWD as a falsification of level preservation.");
    // Closure (the gap_reentry precedent): every offered arc lands in exactly
    // one bucket, so a reader never has to infer a missing cell.
    const long long closure = o.applied + o.true_slip_freed + o.fa_kept + o.fa_freed
                            + o.no_carry + o.no_lambda;
    if (closure != o.seen)
        log->error("Network (slip ORACLE): counter closure BROKEN — {} seen vs {} bucketed "
                   "(applied {} + true_slip_freed {} + fa_kept {} + fa_freed {} + "
                   "no_carry {} + no_lambda {}).",
                   o.seen, closure, o.applied, o.true_slip_freed, o.fa_kept, o.fa_freed,
                   o.no_carry, o.no_lambda);
    // …and the tier split's own closure: an `other` bucket that is nonzero
    // means a call site passed a cause bit this split does not name, i.e. a
    // detector whose false alarms are attributed to nothing.
    const long long tier_sum = o.fa_kept_mw + o.fa_kept_lli + o.fa_kept_iftd
                             + o.fa_kept_wusat + o.fa_kept_other;
    if (tier_sum != o.fa_kept || o.fa_kept_other != 0)
        log->error("Network (slip ORACLE): false-alarm TIER split BROKEN — {} kept vs {} "
                   "attributed (MW {} + LLI {} + IF-TD {} + whole-sat {} + other {}); a "
                   "nonzero 'other' is a call site passing an unrecognised cause bit.",
                   o.fa_kept, tier_sum, o.fa_kept_mw, o.fa_kept_lli, o.fa_kept_iftd,
                   o.fa_kept_wusat, o.fa_kept_other);
    return 0;
}

// ---- End of the run-end reports ----

// Setup builder; documented at its declaration in network/slip/SlipOracle.hpp.
void networkSlipOracleArm(NetworkRun& run, LogManager& log_manager_) {
    // ---- From the run (generated by scripts/nom_ep4_cut.py, each line
    //      checked by scripts/nom_ep4_verify.py). ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const NetworkConfig& nc = run.nc;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    std::vector<NetStation>& stations = run.setup.stations;
    // ---- End of the bindings ----
    // Oracle slip response, simulation only (network.cycle_slip.oracle_relevel,
    // include/network/slip/SlipOracle.hpp): the level-preserving re-level ceiling.
    // It is constructed only when armed; every drain takes a NetworkSlipOracle*
    // that stays null otherwise, so the off path is structurally byte-identical.
    // The load throws on a missing or mis-pointed fixture, which is what keeps
    // the oracle off field data.  The throw propagates to runNetwork's caller like
    // any other fatal setup error; a silent zero-repair run instead would be read
    // as a falsification of the mechanism under test.
    if (nc.slip_qc.oracle_relevel) {
        ctx.carry.slip_oracle =
            networkSlipOracleLoad(config_, stations, log_manager_, log);
        ctx.carry.slip_oracle->carry = &ctx.entities.arcs;
    }
}

} // namespace GnssProcessor
