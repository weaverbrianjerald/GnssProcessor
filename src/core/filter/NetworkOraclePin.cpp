/**
 * @file NetworkOraclePin.cpp
 * @brief Implementation of the sim-only ORACLE AMBIGUITY PIN.  See the header
 *        banner for the card, the gauge argument and the field-inertness
 *        mechanism.
 */

#include "filter/NetworkOraclePin.hpp"

#include "app/NetworkRunContext.hpp"
#include "common/GnssTypes.hpp"
#include "common/PppUtils.hpp"
#include "config/GnssConfig.hpp"
#include "logging/LogManager.hpp"
#include "logging/NetworkRunTraces.hpp"

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

namespace GnssProcessor {
namespace {

/// Trim ASCII whitespace (and a stray CR from a Windows-authored fixture).
std::string opinTrim(std::string s) {
    const auto not_ws = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_ws));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_ws).base(), s.end());
    return s;
}

/// The system letter -> System mapping the wavelength provider keys on.
System opinSystemOf(const std::string& sat) {
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

/// "<sat>_<signal>" — the key form `this_epoch_anchor` / `pinned_amb_targets` use.
std::string opinFamilyKey(const std::string& sat, const std::string& sig) {
    return sat + "_" + sig;
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
        // '+'-separated RINEX-3 PHASE codes.  A non-phase token is refused by
        // NAME: a pin on a code observable's "ambiguity" does not exist, and a
        // silently-empty scope would read as an inert arm.
        std::string tok;
        for (char c : tail + "+") {
            if (c == '+') {
                if (!tok.empty()) {
                    if (tok[0] != 'L') {
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
    // Pinned by NAME, like the slip oracle's, so a fixture that grows a column
    // fails loudly here instead of silently mis-reading N_int.
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
    // Slips are ASCENDING in s; the bound is INCLUSIVE — a jump stamped exactly
    // at this epoch is already in the observation, so the true integer must
    // contain it too.
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

    // Collect the armed events.  The loader already refused a bad scope / sigma
    // and a missing truth root; re-checked here because this function is also
    // the unit suite's entry point (a hand-built config never passed through
    // loadFromJson) — and because a silently-degraded oracle would produce a
    // WRONG SCIENTIFIC VERDICT, not merely a quiet run.
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
            // ⚠ The RUN's wavelength, not the CSV's: FDMA satellites have a
            // per-satellite lambda, and the pin's metres must be the SAME metres
            // the estimator's ambiguity column carries.  An arc whose wavelength
            // the provider cannot resolve is dropped LOUDLY (counted, reported in
            // the arming line) rather than pinned at a wrong scale.
            const double lam = pin->utils->getWavelength(opinSystemOf(key.first),
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
        const std::map<std::tuple<int, std::string, std::string>, LocalCarry>& carry,
        const std::map<std::string, std::string>& anchor_of,
        double now_s,
        const std::vector<std::size_t>& active) {
    pin.table.clear();
    if (active.empty()) return 0;

    const int n_sta = static_cast<int>(stations.size());
    std::map<std::string, int> idx_of;          // station NAME -> index (anchor_of holds names)
    for (int i = 0; i < n_sta; ++i) idx_of[stations[static_cast<std::size_t>(i)].name] = i;

    // ---- the families that have an ANCHOR this epoch ------------------------
    // A family is usable only when its anchor resolves to a roster station AND
    // that station's own arc has truth: without the anchor's true integer there
    // is no frame term, and a pin built on a guessed one would be a gauge of the
    // pin's own making — exactly what this fit exists to avoid.
    struct Fam {
        int    a{-1};             ///< anchor station index
        double target{0.0};       ///< its ENFORCED pin target t_sj (m) — READ, never assumed 0
        double lam_n_a{0.0};      ///< lambda_j * N_true(anchor arc) at this epoch
        double lam{0.0};
    };
    std::map<std::pair<std::string, std::string>, Fam> fams;
    for (const auto& [fam_key, a_name] : anchor_of) {
        const auto us = fam_key.rfind('_');
        if (us == std::string::npos || us == 0 || us + 1 >= fam_key.size()) continue;
        const std::string sat = fam_key.substr(0, us);
        const std::string sig = fam_key.substr(us + 1);
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

    // ---- the observations: one per CARRIED non-anchor arc -------------------
    // Eliminating beta between an arc and its family's anchor leaves a pure
    // STATION-OFFSET DIFFERENCE, with every frame term gone:
    //
    //     g(r,s,j) = value_abs(r,s,j) - t_sj - lambda_j*(N_rsj - N_{a s j})
    //              = v_rj - v_{a(s,j) j}
    //
    // so the fit is a LEVELLING NETWORK over the per-(station, band) offsets v,
    // determined up to ONE constant per band — the free constant the pinned
    // values are invariant to (header banner property 3).
    std::map<std::pair<int, std::string>,
             std::vector<std::pair<int, double>>> obs;   // (sta, band) -> [(anchor idx, g)]
    for (const auto& [key, lc] : carry) {
        const int sta = std::get<0>(key);
        if (sta < 0 || sta >= n_sta) continue;
        const std::string& sat = std::get<1>(key);
        const std::string& sig = std::get<2>(key);
        const auto fit = fams.find({sat, sig});
        if (fit == fams.end()) continue;                 // family has no usable anchor
        if (fit->second.a == sta) continue;              // the anchor's own arc: t_sj IS its level
        const std::string fam_key = opinFamilyKey(sat, sig);
        if (stations[static_cast<std::size_t>(sta)].pinned_amb_targets.count(fam_key))
            continue;                                    // gauge-pinned here too (eq34 / legacy)
        const auto tit = pin.truth.find(key);
        if (tit == pin.truth.end() || !(tit->second.lambda_m > 0.0)) continue;
        if (!std::isfinite(lc.value_abs)) continue;
        const double lam_n_r = tit->second.lambda_m
                             * networkOraclePinTrueN(tit->second, now_s);
        obs[{sta, sig}].emplace_back(
            fit->second.a,
            lc.value_abs - fit->second.target - (lam_n_r - fit->second.lam_n_a));
    }

    // ---- resolve v, band by band, by DETERMINATION (never re-estimation) ----
    // The obvious scheme — alternate "beta from the anchors, u from the medians"
    // to a tolerance — does NOT converge.  Its error recursion is
    // e_r = median over the station's families of e_{anchor(family)}, and a
    // median is a SELECTION, not an average, so the anchors' errors cycle among
    // their own values forever (reproduced on the 6x8x2 unit bed: 20/20
    // iterations, oscillating between two values).
    //
    // Since the pinned values are invariant to the free constant, the fix is to
    // FIX IT: pick a ROOT anchor per band, declare v_root = 0, and PROPAGATE.
    // A station's v is estimated ONCE, from anchors whose v is already known,
    // and is then frozen — so the pass terminates in at most one round per
    // station, and every value is a robust median of >= 3 samples.  The root is
    // deterministic: the anchor owning the most families in that band, ties
    // broken by station index.
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
            // >= 3 arcs: a station-band offset fitted on one or two arcs is not a
            // station term, it is that arc's own error re-labelled — and pinning
            // on it would inject that error into every other arc of the station.
            // A MEDIAN, not a mean: on the slipped bed a single un-repaired arc is
            // a multi-cycle outlier and a mean would drag the whole station with it.
            if (vals.size() < 3) continue;
            v[rb] = opinMedian(vals);
            v_ok.insert(rb);
            any = true;
        }
        if (!any) break;
        ++rounds;
    }

    // ---- build the table ----------------------------------------------------
    // The UNIVERSE is the truth table (every arc of the day), not the carry: an
    // arc in scope with no LocalCarry entry — a newborn leg, an arc the slip
    // response just freed — is still pinned.  That independence is the point.
    //
    //     pinned(r,s,j) = t_sj + (v_rj - v_{a j}) + lambda_j*(N_rsj - N_{a s j})
    //
    // which IS beta_sj + v_rj + lambda_j*N_rsj with
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
                break;                       // the FIRST covering event owns the row
            }
        if (!owner) continue;

        // Never pin an ANCHOR arc: its 1e10 datum pin defines the family's gauge
        // and a second row there would fight it (header banner property 2).
        const std::string fam_key = opinFamilyKey(sat, sig);
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

    // ---- the fit residual = the ACTUATION delta -----------------------------
    // value_abs - pinned, in cycles, over the pinned arcs that HAVE a carry.  It
    // is what the pin is about to move, so an arm whose residual is ~0 pinned a
    // solution that was already at truth, and one with a large residual is
    // measuring exactly how far the float ambiguities sit from the integers.
    double sum2 = 0.0, mx = 0.0;
    long long n = 0;
    for (const auto& [key, p] : pin.table) {
        const auto cit = carry.find(key);
        if (cit == carry.end() || !std::isfinite(cit->second.value_abs)) continue;
        const auto tit = pin.truth.find(key);
        if (tit == pin.truth.end() || !(tit->second.lambda_m > 0.0)) continue;
        const double d = (cit->second.value_abs - p.value_m) / tit->second.lambda_m;
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

void networkOraclePinEpoch(NetworkRunContext& ctx,
                           const GnssConfig& config_,
                           const std::vector<NetStation>& stations,
                           const GnssTime& t,
                           const std::shared_ptr<spdlog::logger>& log,
                           NetworkRunTraces& traces) {
    auto* pin = ctx.carry.oracle_pin.get();
    if (!pin) return;                    // unreachable OFF (the caller tests too)

    const auto& nc   = config_.getNetworkConfig();
    const double tow   = t.tow;
    const double now_s = t.week * 604800.0 + t.tow;   // continuous-GPS seconds

    // PERSISTENT from `tow` on, exactly like sat_clock_step: once a scope has
    // started it stays started for the rest of the run.  Half-epoch guard = the
    // injector's own 0.25 s.
    std::vector<std::size_t> active;
    for (std::size_t i = 0; i < pin->scopes.size(); ++i)
        if (tow >= pin->scopes[i].tow - 0.25) active.push_back(i);

    if (active.empty()) {
        pin->table.clear();
        pin->prev_keys.clear();
        return;                          // nothing due: no counter moves
    }
    ++pin->epochs_active;

    networkOraclePinFit(*pin, stations, ctx.carry.local_carry,
                        ctx.carry.this_epoch_anchor, now_s, active);

    // --- the two edges of invariant #7 — counted AND IDENTIFIED ---------------
    // Both edges get a per-arc event row, not just a counter.  The counter alone
    // was measured to be insufficient: a grader that reconstructs "which arcs are
    // pinned at epoch t" from the ONSET rows alone under-counts by every arc that
    // joined later (7 828 at onset vs 9 381 at the run end on the first quiet
    // arm), and then reads a working pin as a failure.  With the ENTRY row and
    // the EXIT row both emitted, the pinned set at any epoch is exactly
    // (entries − exits) up to that epoch — which is what invariant #7 means by
    // "with counters AND identities".
    //
    // Volume: the onset epoch emits the whole table once; after that only the
    // edges, which are the few tens of arcs per epoch a rising or setting
    // satellite moves.
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
        if (pin->prev_keys.count(k)) continue;           // ENTRY edge only
        const int sta = std::get<0>(k);
        const auto cit = ctx.carry.local_carry.find(k);
        const auto tit = pin->truth.find(k);
        const double lam = (tit != pin->truth.end()) ? tit->second.lambda_m : 0.0;
        const double before = (cit != ctx.carry.local_carry.end())
                                  ? cit->second.value_abs
                                  : std::numeric_limits<double>::quiet_NaN();
        // The ACTUATION delta: how far this arc's carried level sits from the
        // truth-consistent value the pin is about to hold it at.  NaN when the
        // arc has no carry (a newborn leg — pinned anyway, which is the point).
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
        if (keys.count(k)) continue;                      // EXIT edge only
        const int sta = std::get<0>(k);
        traces.emitEvent(t, "INJECT", "oracle_pin", std::get<1>(k),
                         stations[static_cast<std::size_t>(sta)].name,
                         std::nullopt, std::nullopt, "released",
                         fmt::format("signal={}", std::get<2>(k)));
    }
    // Every active epoch: ONE summary row.
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
    // The injector's never-fired ledger (NetworkInject.cpp's run-end rule: an
    // armed event that NEVER fires is a HARD error, because an exam fixture must
    // not silently not exist).  A non-empty pin table IS this kind's actuation.
    // The bounds test is defensive only — the pin is constructed solely when
    // network.inject is enabled with an oracle_pin event, so networkInjectArm has
    // always sized this vector by the time the loop runs.
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

} // namespace GnssProcessor
