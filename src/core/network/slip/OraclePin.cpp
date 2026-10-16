/**
 * @file OraclePin.cpp
 * @brief Implementation of the simulation-only oracle ambiguity pin.  The header's
 *        file comment holds the purpose, the gauge argument and the
 *        field-inertness mechanism.
 */

#include "network/slip/OraclePin.hpp"

#include "network/state/RunContext.hpp"
#include "common/GnssIdentity.hpp"
#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "filter/StateKeySchema.hpp"
#include "logging/LogManager.hpp"
#include "network/diag/RunTraces.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <fstream>
#include <sstream>
#include <stdexcept>

// The run-end reports at the end of this file:
#include "network/runend/RunEnd.hpp"
#include <memory>
#include <spdlog/spdlog.h>
#include "network/driver/Epoch.hpp"   // NetworkEpoch
#include "network/driver/Run.hpp"     // NetworkRun
#include "network/driver/SetupResult.hpp"
#include "rinex/MultiRinexSync.hpp"
#include "network/assembly/StationBuild.hpp"   // StationBuild, StationCarryFrame
#include "network/solve/Estimator.hpp"
#include "network/driver/RunTypes.hpp"
#include "network/assembly/Contributions.hpp"
#include "network/carry/Coupling.hpp"
#include "network/assembly/ParamInventory.hpp"
#include <Eigen/Dense>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "network/solve/PhaseDatum.hpp"   // networkIsGaugePinned

namespace GnssProcessor {
namespace {

/// Trim ASCII whitespace (and a stray CR from a Windows-authored fixture).
std::string opinTrim(std::string s) {
    const auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
    return s;
}

/// In-place median of @p v (v is reordered).  Even n takes the mean of the two
/// central order statistics; the caller guarantees a non-empty vector.
double opinMedian(std::vector<double>& v) {
    const std::size_t n = v.size();
    const std::size_t m = n / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
    const double hi = v[m];
    if (n % 2 == 1) return hi;
    const double lo = *std::max_element(v.begin(),
                                        v.begin() + static_cast<std::ptrdiff_t>(m));
    return 0.5 * (lo + hi);
}

} // namespace

NetworkOraclePin::~NetworkOraclePin() = default;

// ===========================================================================
// Scope
// ===========================================================================

bool OraclePinScope::covers(const std::string& sta_name, const std::string& sat_id,
                            const std::string& sig) const {
    switch (kind) {
        case Kind::All:     return true;
        case Kind::Band:    return signals.count(sig) != 0;
        case Kind::Station: return sta_name == station;
        case Kind::Sat:     return sat_id == sat;
        case Kind::Arc:     return sta_name == station && sat_id == sat && sig == signal;
        case Kind::Invalid: break;
    }
    return false;
}

OraclePinScope oraclePinParseScope(const std::string& s, std::string& why) {
    OraclePinScope out;
    const std::string t = opinTrim(s);
    if (t.empty()) {
        why = "scope is empty (want 'all' | 'band:<sig>[+<sig>...]' | "
              "'station:<name>' | 'sat:<id>' | 'arc:<sta>/<sat>/<sig>')";
        return out;
    }
    if (t == "all") { out.kind = OraclePinScope::Kind::All; return out; }

    const auto colon = t.find(':');
    if (colon == std::string::npos) {
        why = "scope '" + t + "' has no ':' and is not 'all' (want 'all' | "
              "'band:<sig>[+<sig>...]' | 'station:<name>' | 'sat:<id>' | "
              "'arc:<sta>/<sat>/<sig>')";
        return out;
    }
    const std::string head = t.substr(0, colon);
    const std::string tail = opinTrim(t.substr(colon + 1));
    if (tail.empty()) {
        why = "scope '" + t + "' has an empty '" + head + ":' argument";
        return out;
    }

    if (head == "band") {
        // '+'-separated RINEX-3 phase codes.  A token without a leading 'L' is
        // refused with its name: a code observable has no ambiguity to pin, and a
        // scope that silently matched nothing would read as an inert arm.
        std::string tok;
        for (char c : tail + "+") {
            if (c == '+') {
                if (!tok.empty()) {
                    if (obsTypeOf(tok) != ObsType::Phase) {
                        why = "scope '" + t + "': band token '" + tok
                            + "' is not a PHASE signal code (leading L)";
                        return out;
                    }
                    out.signals.insert(tok);
                }
                tok.clear();
            } else if (!std::isspace(static_cast<unsigned char>(c))) {
                tok.push_back(c);
            }
        }
        if (out.signals.empty()) {
            why = "scope '" + t + "': no signal codes after 'band:'";
            return out;
        }
        out.kind = OraclePinScope::Kind::Band;
        return out;
    }
    if (head == "station") {
        out.kind = OraclePinScope::Kind::Station;
        out.station = tail;
        return out;
    }
    if (head == "sat") {
        out.kind = OraclePinScope::Kind::Sat;
        out.sat = tail;
        return out;
    }
    if (head == "arc") {
        const auto s1 = tail.find('/');
        const auto s2 = (s1 == std::string::npos) ? std::string::npos
                                                  : tail.find('/', s1 + 1);
        if (s1 == std::string::npos || s2 == std::string::npos) {
            why = "scope '" + t + "': an arc scope is 'arc:<station>/<sat>/<signal>' "
                  "(two '/' separators)";
            return out;
        }
        out.station = opinTrim(tail.substr(0, s1));
        out.sat     = opinTrim(tail.substr(s1 + 1, s2 - s1 - 1));
        out.signal  = opinTrim(tail.substr(s2 + 1));
        if (out.station.empty() || out.sat.empty() || out.signal.empty()) {
            why = "scope '" + t + "': an arc scope needs a non-empty station, "
                  "satellite AND signal";
            return out;
        }
        out.kind = OraclePinScope::Kind::Arc;
        return out;
    }
    why = "scope '" + t + "': unknown selector '" + head + "' (want 'all' | 'band' | "
          "'station' | 'sat' | 'arc')";
    return out;
}

// ===========================================================================
// CSV
// ===========================================================================

std::map<std::pair<std::string, std::string>, std::pair<int, double>>
networkOraclePinParseAmbCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error(
            "oracle_pin: cannot open truth ambiguity file '" + path + "'");

    std::map<std::pair<std::string, std::string>, std::pair<int, double>> out;

    std::string line;
    if (!std::getline(in, line))
        throw std::runtime_error(
            "oracle_pin: truth ambiguity file '" + path + "' is empty");
    // Header contract (src/sim/main.cpp): satellite,signal,N_int,wavelength_m.
    // Checked by name as a prefix of the header line, as in the slip oracle, so a
    // column inserted before or among these four fails here instead of being
    // misread as N_int; a column appended after wavelength_m passes and is ignored.
    {
        const std::string hdr = opinTrim(line);
        if (hdr.rfind("satellite,signal,N_int,wavelength_m", 0) != 0)
            throw std::runtime_error(
                "oracle_pin: unexpected truth ambiguity header in '" + path
                + "' (want 'satellite,signal,N_int,wavelength_m', got '" + hdr + "')");
    }

    long long row = 1;
    while (std::getline(in, line)) {
        ++row;
        const std::string t = opinTrim(line);
        if (t.empty()) continue;
        std::istringstream ss(t);
        std::string f_sat, f_sig, f_n, f_lam;
        if (!std::getline(ss, f_sat, ',') || !std::getline(ss, f_sig, ',')
            || !std::getline(ss, f_n, ',') || !std::getline(ss, f_lam, ','))
            throw std::runtime_error(fmt::format(
                "oracle_pin: malformed truth ambiguity row {} in '{}' "
                "(want 4 comma-separated fields)", row, path));
        try {
            const std::string sat = opinTrim(f_sat), sig = opinTrim(f_sig);
            if (sat.empty() || sig.empty())
                throw std::runtime_error("empty satellite or signal");
            const int    n   = static_cast<int>(std::lround(std::stod(opinTrim(f_n))));
            const double lam = std::stod(opinTrim(f_lam));
            if (!out.emplace(std::make_pair(sat, sig),
                             std::make_pair(n, lam)).second)
                throw std::runtime_error("duplicate (satellite, signal) arc '"
                                         + sat + "/" + sig + "'");
        } catch (const std::exception& e) {
            throw std::runtime_error(fmt::format(
                "oracle_pin: malformed truth ambiguity row {} in '{}': {}",
                row, path, e.what()));
        }
    }
    return out;
}

int networkOraclePinTrueN(const NetworkOraclePin::Truth& t, double now_s) {
    long long n = t.n_int;
    // Slips are ascending in s, and the bound is inclusive: a jump stamped exactly
    // at this epoch is already in the observation, so the true integer contains it
    // too.
    for (const auto& sl : t.slips) {
        if (sl.s > now_s) break;
        n += sl.delta_cyc;
    }
    return static_cast<int>(n);
}

// ===========================================================================
// Load
// ===========================================================================

std::unique_ptr<NetworkOraclePin> networkOraclePinLoad(
        const GnssConfig& config_,
        const std::vector<NetStation>& stations,
        LogManager& log_manager,
        const std::shared_ptr<spdlog::logger>& log) {
    const auto& nc = config_.getNetworkConfig();

    auto pin = std::make_unique<NetworkOraclePin>();
    pin->truth_root = nc.slip_qc.oracle_truth_root;
    pin->utils      = std::make_unique<PppUtils>(log_manager);

    // Collect the armed events.  The loader already refuses a bad scope or sigma
    // and a missing truth root.  The checks are repeated here because this function
    // is also the unit suite's entry point (a hand-built config does not pass
    // through loadFromJson), and because a silently degraded oracle would produce a
    // wrong scientific verdict, not only a quiet run.
    for (std::size_t i = 0; i < nc.inject.events.size(); ++i) {
        const auto& e = nc.inject.events[i];
        if (e.kind != "oracle_pin") continue;
        std::string why;
        NetworkOraclePin::Armed a;
        a.ev_index = i;
        a.scope    = oraclePinParseScope(e.scope, why);
        a.sigma_m  = e.sigma_m;
        a.tow      = e.tow;
        if (a.scope.kind == OraclePinScope::Kind::Invalid)
            throw std::runtime_error(
                "network.inject.events[" + std::to_string(i)
                + "] (kind 'oracle_pin'): " + why);
        if (!(a.sigma_m > 0.0) || !std::isfinite(a.sigma_m))
            throw std::runtime_error(
                "network.inject.events[" + std::to_string(i)
                + "] (kind 'oracle_pin'): sigma_m must be > 0 and finite");
        pin->scopes.push_back(std::move(a));
    }
    if (pin->scopes.empty())
        throw std::runtime_error(
            "oracle_pin: networkOraclePinLoad called with no 'oracle_pin' event armed "
            "— the caller must test for one first (the OFF path must never construct "
            "this object).");

    const std::filesystem::path root(pin->truth_root);
    if (pin->truth_root.empty() || !std::filesystem::exists(root))
        throw std::runtime_error(
            "oracle_pin: network.cycle_slip.oracle_truth_root '" + pin->truth_root
            + "' does not exist.  This is a SIM-ONLY DIAGNOSTIC that reads a simulator "
              "fixture's truth_ambiguities.csv / truth_slips.csv; it is field-inert by "
              "construction and this error is that mechanism.");

    long long n_arcs = 0, n_slips = 0, n_nolam = 0;
    for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
        const std::string& name = stations[static_cast<std::size_t>(i)].name;
        const auto amb_file  = root / name / "truth_ambiguities.csv";
        const auto slip_file = root / name / "truth_slips.csv";
        for (const auto* f : {&amb_file, &slip_file})
            if (!std::filesystem::exists(*f))
                throw std::runtime_error(
                    "oracle_pin: no truth file for ROSTER station '" + name + "' at '"
                    + f->string()
                    + "'.  A sim fixture writes one per station, so this is a "
                      "mis-pointed oracle_truth_root (or a roster that is not this "
                      "fixture's), not a quiet station — refusing to run a pin that "
                      "would be silently partial.");
        const auto ambs  = networkOraclePinParseAmbCsv(amb_file.string());
        const auto slips = networkSlipOracleParseCsv(slip_file.string());
        for (const auto& [key, nv] : ambs) {
            NetworkOraclePin::Truth t;
            t.n_int        = nv.first;
            t.lambda_csv_m = nv.second;
            const auto sit = slips.find(key);
            if (sit != slips.end()) {
                t.slips = sit->second;
                n_slips += static_cast<long long>(t.slips.size());
            }
            // Use the run's wavelength, not the CSV's: FDMA satellites have a
            // per-satellite lambda, and the pin's metres must be the metres the
            // estimator's ambiguity column carries.  An arc whose wavelength the
            // provider cannot resolve is dropped and counted (reported in the arming
            // line) instead of being pinned at a wrong scale.
            const double lam = pin->utils->getWavelength(systemOfSat(key.first),
                                                         key.second, key.first);
            if (!(lam > 0.0)) { ++n_nolam; continue; }
            t.lambda_m = lam;
            pin->truth[{i, key.first, key.second}] = std::move(t);
            ++n_arcs;
        }
    }

    std::string list;
    for (const auto& a : pin->scopes)
        list += fmt::format("{}#{} '{}' sigma {:.2e} m @tow {:.1f}",
                            list.empty() ? "" : "; ", a.ev_index,
                            nc.inject.events[a.ev_index].scope, a.sigma_m, a.tow);
    log->warn("Network (ORACLE PIN): {} 'oracle_pin' event(s) ARMED — a SIM-ONLY "
              "DIAGNOSTIC.  The producer's float ambiguities in scope are HELD at the "
              "fixture's true integers from '{}': {} arc(s) with truth across {} roster "
              "station(s), carrying {} truth slip(s); {} arc(s) dropped with no "
              "wavelength.  [{}]  This run's products are a DIAGNOSTIC FIXTURE, never a "
              "solution: the pin is field-inert by construction and must never appear in "
              "a golden or the promotion ledger.  docs/SPEC_oracle_pin.md",
              pin->scopes.size(), pin->truth_root, n_arcs, stations.size(), n_slips,
              n_nolam, list);
    return pin;
}

// ===========================================================================
// The fit
// ===========================================================================

int networkOraclePinFit(
        NetworkOraclePin& pin,
        const std::vector<NetStation>& stations,
        const net::ArcTable& arcs,
        const std::map<std::string, std::string>& anchor_of,
        double now_s,
        const std::vector<std::size_t>& active) {
    pin.table.clear();
    if (active.empty()) return 0;

    const int n_sta = static_cast<int>(stations.size());
    std::map<std::string, int> idx_of;          // station name -> index (anchor_of holds names)
    for (int i = 0; i < n_sta; ++i) idx_of[stations[static_cast<std::size_t>(i)].name] = i;

    // ---- the families that have an anchor this epoch ------------------------
    // A family is usable only when its anchor resolves to a roster station and
    // that station's own arc has truth.  Without the anchor's true integer there
    // is no frame term, and a pin built on a guessed one would impose a gauge of
    // its own, which this fit is designed to avoid.
    struct Fam {
        int    a{-1};             ///< anchor station index
        double target{0.0};       ///< its enforced pin target t_sj (m); read, not assumed 0
        double lam_n_a{0.0};      ///< lambda_j * N_true(anchor arc) at this epoch
        double lam{0.0};
    };
    std::map<std::pair<std::string, std::string>, Fam> fams;
    for (const auto& [fam_key, a_name] : anchor_of) {
        // fam_key is the family's ambiguity key "<sat>_<signal>" (StateKeySchema).
        const auto parts = StateKeys::parseAmbiguityKey(fam_key);
        if (!parts) continue;
        const std::string& sat = parts->satellite;
        const std::string& sig = parts->signal;
        const auto iit = idx_of.find(a_name);
        if (iit == idx_of.end()) continue;
        const auto tit = pin.truth.find({iit->second, sat, sig});
        if (tit == pin.truth.end() || !(tit->second.lambda_m > 0.0)) continue;
        Fam f;
        f.a = iit->second;
        const auto& sta_a = stations[static_cast<std::size_t>(f.a)];
        const auto pit = sta_a.pinned_amb_targets.find(fam_key);
        f.target  = (pit != sta_a.pinned_amb_targets.end()) ? pit->second : 0.0;
        f.lam     = tit->second.lambda_m;
        f.lam_n_a = f.lam * networkOraclePinTrueN(tit->second, now_s);
        fams[{sat, sig}] = f;
    }

    // ---- the observations: one per carried non-anchor arc -------------------
    // Eliminating beta between an arc and its family's anchor leaves a pure
    // station-offset difference, with every frame term gone:
    //
    //     g(r,s,j) = value_abs(r,s,j) - t_sj - lambda_j*(N_rsj - N_{a s j})
    //              = v_rj - v_{a(s,j) j}
    //
    // so the fit is a levelling network over the per-(station, band) offsets v
    // (u in the header), determined up to one constant per band; the pinned values
    // are invariant to that constant (header, property 3).
    std::map<std::pair<int, std::string>,
             std::vector<std::pair<int, double>>> obs;   // (sta, band) -> [(anchor idx, g)]
    net::forEach(arcs, &net::ArcState::amb, [&](const ArcId& id, const LocalCarry& lc) {
        const net::ArcTuple key = net::arcTuple(id);
        const int sta = std::get<0>(key);
        if (sta < 0 || sta >= n_sta) return;
        const std::string& sat = std::get<1>(key);
        const std::string& sig = std::get<2>(key);
        const auto fit = fams.find({sat, sig});
        if (fit == fams.end()) return;                 // family has no usable anchor
        if (fit->second.a == sta) return;              // the anchor's own arc: t_sj is its level
        const std::string fam_key = StateKeys::makeAmbiguityKey(sat, sig);
        if (stations[static_cast<std::size_t>(sta)].pinned_amb_targets.count(fam_key))
            return;                                    // gauge-pinned here too (eq34 / legacy)
        const auto tit = pin.truth.find(key);
        if (tit == pin.truth.end() || !(tit->second.lambda_m > 0.0)) return;
        if (!std::isfinite(lc.value_abs)) return;
        const double lam_n_r = tit->second.lambda_m
                             * networkOraclePinTrueN(tit->second, now_s);
        obs[{sta, sig}].emplace_back(
            fit->second.a,
            lc.value_abs - fit->second.target - (lam_n_r - fit->second.lam_n_a));
    });

    // ---- resolve v, band by band, by determination (no re-estimation) -------
    // Alternating "beta from the anchors, u from the medians" to a tolerance does
    // not converge.  Its error recursion is
    // e_r = median over the station's families of e_{anchor(family)}; a median
    // selects one sample instead of averaging, so the anchors' errors cycle among
    // their own values indefinitely (on the 6x8x2 unit bed: 20 of 20 iterations,
    // oscillating between two values).
    //
    // The pinned values are invariant to the free constant, so the constant is
    // fixed instead: one root anchor per band gets v_root = 0, and values
    // propagate from it.  A station's v is estimated once, from anchors whose v is
    // already known, and then frozen, so the pass ends after at most one round per
    // station and every value is a median of >= 3 samples.  The root is
    // deterministic: the anchor owning the most families in that band, ties broken
    // by lowest station index.
    std::map<std::pair<int, std::string>, double> v;
    std::set<std::pair<int, std::string>> v_ok;
    {
        std::map<std::string, std::map<int, long long>> anchors_per_band;
        for (const auto& [key, f] : fams) ++anchors_per_band[key.second][f.a];
        for (const auto& [band, counts] : anchors_per_band) {
            int root = -1; long long best = -1;
            for (const auto& [a, n] : counts)          // std::map => ascending index
                if (n > best) { best = n; root = a; }  // strict > => the lowest index wins a tie
            if (root >= 0) { v[{root, band}] = 0.0; v_ok.insert({root, band}); }
        }
    }
    int rounds = 0;
    for (int it = 0; it < n_sta + 1; ++it) {
        bool any = false;
        for (auto& [rb, samples] : obs) {
            if (v_ok.count(rb)) continue;
            std::vector<double> vals;
            vals.reserve(samples.size());
            for (const auto& [a, g] : samples) {
                const auto vit = v.find({a, rb.second});
                if (vit != v.end()) vals.push_back(g + vit->second);
            }
            // At least 3 arcs: a station-band offset fitted on one or two arcs is
            // that arc's own error under another name, and pinning on it would
            // inject that error into every other arc of the station.  A median, not
            // a mean: on the slipped bed a single unrepaired arc is a multi-cycle
            // outlier, and a mean would shift the whole station with it.
            if (vals.size() < 3) continue;
            v[rb] = opinMedian(vals);
            v_ok.insert(rb);
            any = true;
        }
        if (!any) break;
        ++rounds;
    }

    // ---- build the table ----------------------------------------------------
    // The loop runs over the truth table (every arc of the day), not the carry, so
    // an arc in scope with no LocalCarry entry (a newborn leg, an arc the slip
    // response just freed) is still pinned; the pin is independent of the carry by
    // design.
    //
    //     pinned(r,s,j) = t_sj + (v_rj - v_{a j}) + lambda_j*(N_rsj - N_{a s j})
    //
    // which equals beta_sj + v_rj + lambda_j*N_rsj with
    // beta_sj = t_sj - lambda_j*N_{a s j} - v_{a j}, written so that every term
    // is a difference and the free constant cancels by construction.
    for (const auto& [key, t] : pin.truth) {
        const int sta = std::get<0>(key);
        if (sta < 0 || sta >= n_sta) continue;
        const std::string& sat = std::get<1>(key);
        const std::string& sig = std::get<2>(key);
        const std::string& sta_name = stations[static_cast<std::size_t>(sta)].name;

        const NetworkOraclePin::Armed* owner = nullptr;
        for (std::size_t ai : active)
            if (pin.scopes[ai].scope.covers(sta_name, sat, sig)) {
                owner = &pin.scopes[ai];
                break;                       // the first covering event owns the row
            }
        if (!owner) continue;

        // Never pin an anchor arc: its 1e10 datum pin defines the family's gauge,
        // and a second row there would conflict with it (header, property 2).
        const std::string fam_key = StateKeys::makeAmbiguityKey(sat, sig);
        const auto ait = anchor_of.find(fam_key);
        if (ait != anchor_of.end() && ait->second == sta_name) continue;
        if (stations[static_cast<std::size_t>(sta)].pinned_amb_targets.count(fam_key))
            continue;

        if (!(t.lambda_m > 0.0)) { ++pin.no_truth; continue; }
        const auto fit = fams.find({sat, sig});
        if (fit == fams.end()) { ++pin.no_anchor; continue; }
        const std::pair<int, std::string> rb{sta, sig};
        const std::pair<int, std::string> ab{fit->second.a, sig};
        if (!v_ok.count(rb) || !v_ok.count(ab)) { ++pin.no_station_term; continue; }

        NetworkOraclePin::Pin p;
        p.n_true   = networkOraclePinTrueN(t, now_s);
        p.value_m  = fit->second.target + (v[rb] - v[ab])
                   + (t.lambda_m * p.n_true - fit->second.lam_n_a);
        p.sigma_m  = owner->sigma_m;
        p.ev_index = owner->ev_index;
        pin.table.emplace(key, p);
    }

    // ---- the fit residual, which is the actuation delta ---------------------
    // value_abs - pinned, in cycles, over the pinned arcs that have a carry entry.
    // It is what the pin is about to move: a residual near 0 means the solution
    // was already at truth, and a large one measures how far the float
    // ambiguities sit from the integers.
    double sum2 = 0.0, mx = 0.0;
    long long n = 0;
    for (const auto& [key, p] : pin.table) {
        const LocalCarry* cit = net::getArc(arcs, key, &net::ArcState::amb);
        if (cit == nullptr || !std::isfinite(cit->value_abs)) continue;
        const auto tit = pin.truth.find(key);
        if (tit == pin.truth.end() || !(tit->second.lambda_m > 0.0)) continue;
        const double d = (cit->value_abs - p.value_m) / tit->second.lambda_m;
        sum2 += d * d;
        mx = std::max(mx, std::abs(d));
        ++n;
    }
    pin.fit_resid_n       = n;
    pin.fit_resid_rms_cyc = (n > 0) ? std::sqrt(sum2 / static_cast<double>(n)) : 0.0;
    pin.fit_resid_max_cyc = mx;
    pin.fit_iters_last    = rounds;
    pin.fit_iters_total  += rounds;
    return rounds;
}

// ===========================================================================
// Per-epoch driver
// ===========================================================================

void networkOraclePinEpoch(NetworkRun& run, const NetworkEpoch& ep) {
    // ---- From the run and the epoch (generated by scripts/nom_ep1_cut.py,
    //      each line checked by scripts/nom_ep1_verify.py). Reads bind through `ep`; writes bind
    //      through the output parameters and are marked `// written`. ----
    NetworkRunContext& ctx = run.ctx;
    const GnssConfig& config_ = run.config;
    const std::vector<NetStation>& stations = run.setup.stations;
    const GnssTime& t = ep.se.time;
    const std::shared_ptr<spdlog::logger>& log = run.log;
    NetworkRunTraces& traces = run.traces;
    const std::map<std::string, std::string>& this_epoch_anchor = ep.events.phase_datum.this_epoch_anchor;
    // ---- End of the bindings ----
    auto* pin = ctx.carry.oracle_pin.get();
    if (!pin) return;                    // the gate: no oracle_pin event (the driver's test went in card NOM3 step EP2)

    const auto& nc   = config_.getNetworkConfig();
    const double tow   = t.tow;
    const double now_s = t.week * 604800.0 + t.tow;   // continuous-GPS seconds

    // Persistent from `tow` on, as for sat_clock_step: once a scope has started it
    // stays started for the rest of the run.  The 0.25 s tolerance is the
    // injector's own.
    std::vector<std::size_t> active;
    for (std::size_t i = 0; i < pin->scopes.size(); ++i)
        if (tow >= pin->scopes[i].tow - 0.25) active.push_back(i);

    if (active.empty()) {
        pin->table.clear();
        pin->prev_keys.clear();
        return;                          // nothing due: no counter moves
    }
    ++pin->epochs_active;

    networkOraclePinFit(*pin, stations, ctx.entities.arcs,
                        this_epoch_anchor, now_s, active);

    // --- the two edges of invariant #7, counted and identified ----------------
    // Both edges get a per-arc event row as well as a counter.  Counters alone are
    // not enough: the set of arcs pinned at epoch t, reconstructed from the onset
    // rows alone, misses every arc that joined later, and a working pin then reads
    // as a failure.  With entry and exit rows both emitted, the pinned set at any
    // epoch is (entries − exits) up to that epoch, which is what invariant #7 asks
    // for ("counters and identities").
    //
    // Volume: the onset epoch emits the whole table once; after that only the
    // edges, a few tens of arcs per epoch as satellites rise and set.
    std::set<std::tuple<int, std::string, std::string>> keys;
    long long first_here = 0;
    for (const auto& [k, p] : pin->table) {
        keys.insert(k);
        if (pin->ever_pinned.insert(k).second) { ++pin->arcs_first_pinned; ++first_here; }
    }
    long long released_here = 0;
    for (const auto& k : pin->prev_keys)
        if (!keys.count(k)) { ++pin->arcs_released; ++released_here; }

    // --- telemetry -----------------------------------------------------------
    const bool onset = (pin->epochs_active == 1);
    for (const auto& [k, p] : pin->table) {
        if (pin->prev_keys.count(k)) continue;           // entry edge only
        const int sta = std::get<0>(k);
        const auto cit = net::getArc(ctx.entities.arcs, k, &net::ArcState::amb);
        const auto tit = pin->truth.find(k);
        const double lam = (tit != pin->truth.end()) ? tit->second.lambda_m : 0.0;
        const double before = (cit != nullptr)
                                  ? cit->value_abs
                                  : std::numeric_limits<double>::quiet_NaN();
        // The actuation delta: how far this arc's carried level sits from the
        // truth-consistent value the pin is about to hold it at.  NaN when the arc
        // has no carry entry (a newborn leg, which is pinned all the same).
        const double dcyc = (lam > 0.0 && std::isfinite(before))
                                ? (before - p.value_m) / lam
                                : std::numeric_limits<double>::quiet_NaN();
        traces.emitEvent(t, "INJECT", "oracle_pin", std::get<1>(k),
                         stations[static_cast<std::size_t>(sta)].name,
                         std::isfinite(dcyc) ? std::optional<double>(dcyc)
                                             : std::nullopt,
                         std::nullopt, "pinned",
                         fmt::format("scope={};signal={};before={:.6f};pinned={:.6f};"
                                     "delta_cyc={:.4f};N={}",
                                     nc.inject.events[p.ev_index].scope,
                                     std::get<2>(k), before, p.value_m, dcyc,
                                     p.n_true));
    }
    for (const auto& k : pin->prev_keys) {
        if (keys.count(k)) continue;                      // exit edge only
        const int sta = std::get<0>(k);
        traces.emitEvent(t, "INJECT", "oracle_pin", std::get<1>(k),
                         stations[static_cast<std::size_t>(sta)].name,
                         std::nullopt, std::nullopt, "released",
                         fmt::format("signal={}", std::get<2>(k)));
    }
    // One summary row per active epoch (the no_* counts are run totals).
    traces.emitEvent(t, "INJECT", "oracle_pin", std::string{}, std::string{},
                     static_cast<double>(pin->table.size()), std::nullopt,
                     "oracle_pin_epoch",
                     fmt::format("pinned={};first={};released={};fit_iters={};"
                                 "resid_rms_cyc={:.4f};resid_max_cyc={:.4f};resid_n={};"
                                 "no_anchor={};no_station_term={};no_truth={}",
                                 pin->table.size(), first_here, released_here,
                                 pin->fit_iters_last, pin->fit_resid_rms_cyc,
                                 pin->fit_resid_max_cyc, pin->fit_resid_n,
                                 pin->no_anchor, pin->no_station_term, pin->no_truth));

    if (onset)
        log->warn("Network (ORACLE PIN): TOW {:.1f} — FIRST active epoch, {} arc(s) "
                  "pinned at truth (fit {} iter(s), residual rms {:.3f} / max {:.3f} cyc "
                  "over {} carried arc(s)).  {} in scope had no anchored family, {} no "
                  "fitted station term, {} no truth.",
                  tow, pin->table.size(), pin->fit_iters_last, pin->fit_resid_rms_cyc,
                  pin->fit_resid_max_cyc, pin->fit_resid_n, pin->no_anchor,
                  pin->no_station_term, pin->no_truth);

    pin->prev_keys = std::move(keys);
    // Feed the injector's never-fired ledger (Inject.cpp run-end rule: an
    // armed event that never fires is logged as a failure and the check returns
    // nonzero, because a test fixture must not silently be absent).  For this
    // kind, a non-empty pin table counts as actuation.  The bounds test is
    // defensive only: the pin is constructed only when network.inject is enabled
    // with an oracle_pin event, so networkInjectArm has already sized this vector.
    if (!pin->table.empty())
        for (std::size_t ai : active) {
            const std::size_t ev = pin->scopes[ai].ev_index;
            if (ev < ctx.inject.n_applied.size()) ++ctx.inject.n_applied[ev];
        }
}

// ===========================================================================
// Run end
// ===========================================================================

int networkOraclePinRunEnd(const NetworkOraclePin* pin,
                           const std::shared_ptr<spdlog::logger>& log) {
    if (!pin) return 0;
    const long long offered = pin->rows_injected + pin->gauge_pinned_skipped
                            + pin->no_column;
    log->warn("Network (ORACLE PIN): {} active epoch(s); {} arc-epoch prior row(s) "
              "INJECTED, {} arc(s) first pinned, {} released; PASS-B offers {} = "
              "injected {} + gauge-pinned skipped {} + no column {}; table-build skips: "
              "no anchor {}, no station term {}, no truth {}; fit {} iteration(s) total "
              "({} on the last epoch), last-epoch residual rms {:.4f} / max {:.4f} cyc "
              "over {} carried arc(s); truth root '{}'.  SIM-ONLY DIAGNOSTIC — this "
              "run's ambiguities are HELD AT TRUTH and its products are NOT a shippable "
              "posture.{}",
              pin->epochs_active, pin->rows_injected, pin->arcs_first_pinned,
              pin->arcs_released, offered, pin->rows_injected,
              pin->gauge_pinned_skipped, pin->no_column, pin->no_anchor,
              pin->no_station_term, pin->no_truth, pin->fit_iters_total,
              pin->fit_iters_last, pin->fit_resid_rms_cyc, pin->fit_resid_max_cyc,
              pin->fit_resid_n, pin->truth_root,
              (pin->rows_injected > 0)
                  ? ""
                  : "  ⚠ ARMED AND INERT: ZERO prior rows were folded — this is dead "
                    "wiring, not a quiet run, and the arm measures NOTHING.  Do NOT read "
                    "its parameter errors as a ceiling.");
    return 0;
}

// ---- Run-end reports (generated by scripts/nom_td_cut.py) ----

// Run-end report: the sim-only oracle ambiguity pin.
int networkOraclePinRunEnd(const NetworkRunEndView& v) {
    const NetworkRunContext& ctx = v.ctx;
    const std::shared_ptr<spdlog::logger>& log = v.log;

    // ---- Sim-only oracle ambiguity pin (include/network/slip/OraclePin.hpp) ----
    // The injected-row count is the only way to tell a working pin from dead wiring
    // (invariant #4).  A run reporting 0 has not measured a ceiling on what integer
    // information buys; it has measured an unpinned run, and the line says so in the
    // log so the run is not graded as the former.
    return networkOraclePinRunEnd(ctx.carry.oracle_pin.get(), log);
}

// ---- End of the run-end reports ----

// ---- PASS B station steps (generated by scripts/nom_pb1b_cut.py) ----

void networkOraclePinPriors(StationCarryFrame& carry) {
    // ---- From the station build (generated by scripts/nom_pb1b_cut.py,
    //      each line checked by scripts/nom_pb1b_verify.py). ----
    StationBuild& build = carry.build;
    const NetworkRunContext& ctx = build.ctx;
    const NetworkSetupResult& setup = build.setup;
    const int i = build.i;
    NetStation& st = build.st;
    StationAssembly& R = build.R;
    std::map<int, int>& h2local = R.h2local;
    const bool legacy_local_inject = carry.legacy_local_inject;
    const Eigen::VectorXd& x_lin = carry.x_lin;
    std::vector<std::tuple<Eigen::Index, double, double, RowTag>>& inj = carry.inj;
    // The alias lines of networkPassB_ this function reads, verbatim.
    auto& stations = setup.stations;
    const bool phase_datum_prioritized = ctx.knobs.phase_datum_prioritized;
    // Simulation-only oracle ambiguity pin (filter/NetworkOraclePin): this epoch's
    // pin table, built serially before PASS B and only read here.  A const pointer,
    // null on every run with no `oracle_pin` event armed, so the produce loop's pin
    // block is unreachable when off (standing invariant #6).
    const std::map<std::tuple<int, std::string, std::string>,
                   NetworkOraclePin::Pin>* opin_table =
        ctx.carry.oracle_pin ? &ctx.carry.oracle_pin->table : nullptr;
    const bool   opin_on = (opin_table != nullptr) && !opin_table->empty();
    // ---- End of the bindings ----

    // --- Simulation-only oracle ambiguity pin (filter/NetworkOraclePin) --
    // One diagonal local prior row per pinned ambiguity column, of the same
    // shape as pl_emit_row's diagonal branch: a real (Λ, η) pair, never an
    // `x_lin` shift (docs/SPEC_lane_slip_response.md §7.2: a shift of x_lin
    // alone is an exact solve no-op, which is why the pin does not also
    // write LocalCarry::value_abs).
    //
    // Its own loop, outside `do_amb_carry` by design: the pin is independent
    // of the leveling carry.  It fires whether or not the arc has a
    // LocalCarry entry (a newborn leg is pinned too), whether or not the slip
    // response freed the arc this epoch (free_ambs), and regardless of the F3
    // block route; that independence is the purpose of the instrument.  The
    // table is built serially once per epoch (networkOraclePinEpoch) and is
    // read-only here, so this is safe in the parallel produce phase.
    // Counters are produce-phase local and folded serially below, like
    // relevel_free_skips.
    //
    // Closure stated by the run-end clause: offered = rows + gauge-pinned
    // skipped + no column.
    if (!(legacy_local_inject && opin_on)) return;
    for (const auto& [key, sidx] : st.sm->getAmbiguities()) {
        const auto parts = StateKeys::parseAmbiguityKey(key);
        if (!parts) continue;
        const auto pit = opin_table->find(
            std::make_tuple(static_cast<int>(i), parts->satellite,
                            parts->signal));
        if (pit == opin_table->end()) continue;
        ++R.opin_offered;
        if (networkIsGaugePinned(stations, phase_datum_prioritized,
                                  i, parts->satellite, parts->signal)) {
            // The anchor's 1e10 datum pin defines this family's gauge;
            // a second row here would fight it.  The table build skips
            // anchors too; this check is the authoritative backstop, and
            // it is counted so the closure never has to be inferred.
            ++R.opin_gauge_skipped;
            continue;
        }
        const int amb_idx = static_cast<int>(sidx);
        if (amb_idx < 0 || amb_idx >= x_lin.size()) {
            ++R.opin_no_column; continue;
        }
        const auto hit = h2local.find(amb_idx);
        if (hit == h2local.end()) {       // column not in A_r this epoch
            ++R.opin_no_column; continue;
        }
        if (!std::isfinite(x_lin(amb_idx))) { ++R.opin_no_column; continue; }
        const double lam = 1.0 / (pit->second.sigma_m * pit->second.sigma_m);
        const double eta = lam * (pit->second.value_m - x_lin(amb_idx));
        if (!std::isfinite(lam) || !std::isfinite(eta)) {
            ++R.opin_no_column; continue;  // a non-finite row must not reach u_r
        }
        inj.emplace_back(static_cast<Eigen::Index>(hit->second), lam, eta,
                         makeRowTag(ContributionKind::PriorAmbiguityOraclePin,
                                    parts->satellite, parts->signal));
        ++R.opin_rows;
    }
}

// ---- End of the PASS B station steps ----

} // namespace GnssProcessor
