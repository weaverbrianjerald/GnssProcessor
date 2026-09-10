/**
 * @file NetworkSlipOracle.cpp
 * @brief Implementation of the sim-only ORACLE slip response (the
 *        level-preserving re-level CEILING).  See the header banner for the
 *        card, the window semantics and the field-inertness argument.
 */

#include "slip/NetworkSlipOracle.hpp"

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

namespace GnssProcessor {
namespace {

/// Trim ASCII whitespace (and a stray CR from a Windows-authored fixture).
std::string trim(std::string s) {
    const auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
    return s;
}

/// The system letter -> System mapping the wavelength provider keys on.
System systemOf(const std::string& sat) {
    if (sat.empty()) return System::GPS;
    switch (sat[0]) {
        case 'G': return System::GPS;
        case 'R': return System::GLONASS;
        case 'E': return System::Galileo;
        case 'C': return System::BeiDou;
        case 'J': return System::QZSS;
        default:  return System::GPS;
    }
}

/// splitmix64 — a full-avalanche 64-bit mixer, so the seed differs in every bit
/// for two arcs that differ in one character.
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
    // Pinned by NAME rather than by position so a fixture that grows a column
    // fails loudly here instead of silently mis-reading delta_cycles.
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
            // week*604800 + tow (slip/NetworkSlipQc.cpp's windup drain).
            out[{sat, sig}].push_back({week * 604800.0 + tow, del});
        } catch (const std::exception& e) {
            throw std::runtime_error(fmt::format(
                "network.cycle_slip.oracle_relevel: malformed truth slip row {} in '{}': {}",
                row, path, e.what()));
        }
    }
    // The fixture writes in time order, but the oracle's window scan assumes it
    // rather than hoping for it: a stable sort keeps equal-stamped rows (two
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
    // The loader already clamped this to >= 0; re-clamped because this function is
    // also the unit suite's entry point (a hand-built config never passed through
    // loadFromJson).  A negative window would make EVERY actuation a false alarm.
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
    // Deterministic seed: the arc identity plus the epoch, so the SAME key drawn
    // twice (two detectors firing on one arc in one epoch, or a re-run of the
    // whole arm) gets the SAME draw.  The epoch is quantized to the millisecond
    // so a float-formatted tow cannot perturb it.
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

    // (1) Nothing to preserve.  The whole card is about the CARRIED level; an arc
    //     with no leveling entry is re-estimated free either way, so today's
    //     response is already the right one.
    if (!oracle.carry) { ++oracle.no_carry; return false; }
    const auto cit = oracle.carry->find(key);
    if (cit == oracle.carry->end()) { ++oracle.no_carry; return false; }

    // (2) The window: (max(last_applied, carry.birth_s, now_s - window_s), now_s].
    //     See the header banner for why each term is there.  In particular the
    //     lower edge is NOT carry.last_seen_s: that is the PREVIOUS epoch's stash
    //     time, and a zero-q carried level is not re-set by the intervening solve,
    //     so a detector firing 1-3 epochs late (the normal latency) is still
    //     looking at an un-absorbed jump.
    auto& arc = oracle.arcs[key];
    const double w_edge = now_s - oracle.window_s;
    const double b_edge = cit->second.birth_s;       // 0 when unknown (warm resume)
    const double lower  = std::max({arc.last_applied_s, b_edge, w_edge});
    // Which term BOUND (invariant #4).  Ties go to the most specific explanation,
    // so a reader can tell "the window is too narrow" from "the arc is younger
    // than the window" from "we already paid this jump" — three different fixes.
    if (lower == arc.last_applied_s)      ++oracle.bound_applied;
    else if (lower == b_edge)             ++oracle.bound_birth;
    else                                  ++oracle.bound_window;
    long long n = 0;
    double    sum_cyc = 0.0;
    const auto tit = oracle.truth.find(key);
    if (tit != oracle.truth.end()) {
        for (const auto& ts : tit->second) {
            if (ts.s <= lower) continue;
            if (ts.s > now_s) break;              // ascending — nothing later qualifies
            sum_cyc += static_cast<double>(ts.delta_cyc);
            ++n;
        }
    }

    // (3) FALSE ALARM — the detector fired and truth says nothing jumped on this
    //     arc in the window.  `keep` is the ceiling (an oracle knows); `free`
    //     falls through so the arm keeps today's false-alarm cost.
    if (n == 0) {
        if (oracle.false_alarm == NetworkSlipOracle::FalseAlarm::Free) {
            ++oracle.fa_freed;
            return false;
        }
        ++oracle.fa_kept;
        // Per-TIER attribution of the keeps (invariant #4).  The decision is
        // already made — the tier only says WHICH detector's thresholds a
        // false-alarm-isolation arm should be read against.  An unrecognised bit
        // is a mis-wired call site, not a fifth detector, so it gets its own
        // bucket rather than being folded into one of the four.
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

    // (3b) TRUE SLIP, policy `free` — the FALSE-ALARM ISOLATION arm.  Truth says
    //      this arc really jumped, so it gets today's ORDINARY response: decline,
    //      and the caller frees / re-levels exactly as it would on the base arm.
    //      Deliberately BEFORE the wavelength lookup — no shift is formed, so a
    //      signal with no wavelength is not a `no_lambda` fall-through here, it
    //      is the same decline for the same reason (and counting it twice would
    //      break the closure).  `last_applied_s` is NOT advanced: nothing was
    //      applied, and the next detector on this arc must see the same window.
    if (oracle.true_slip == NetworkSlipOracle::TrueSlip::Free) {
        ++oracle.true_slip_freed;
        return false;
    }

    // (4) REPAIR.  Wavelength in the signal's OWN band (delta_cycles is in that
    //     signal's cycles), FDMA-correct via the satellite id.  An unknown signal
    //     is counted and falls through — never silently skipped, which would read
    //     as a false alarm and corrupt the census.
    const double lambda =
        oracle.utils ? oracle.utils->getWavelength(systemOf(sat), signal, sat) : 0.0;
    if (!(lambda > 0.0) || !std::isfinite(lambda)) { ++oracle.no_lambda; return false; }

    const double noise = networkSlipOracleNoise(oracle.noise_cyc, sta, sat, signal, now_s);
    // ⚠ value_abs ONLY.  The variance is DELIBERATELY unchanged: the point of the
    // ceiling is that a perfect repair costs no information, so the level stays
    // at its converged weight (an x_lin shift, by contrast, is an exact solve
    // no-op — docs/SPEC_lane_slip_response.md §7.2).
    cit->second.value_abs += (sum_cyc + noise) * lambda;
    arc.last_applied_s = now_s;

    ++oracle.applied;
    if (n >= 2) ++oracle.multi;
    oracle.sum_abs_cyc += std::abs(sum_cyc);
    oracle.max_abs_cyc  = std::max(oracle.max_abs_cyc, std::abs(sum_cyc));

    detail  = fmt::format("delta={};n={};noise={:.4f}", sum_cyc, n, noise);
    outcome = OracleOutcome::Repaired;
    return true;
}

} // namespace GnssProcessor
