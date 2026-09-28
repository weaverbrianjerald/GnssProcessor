/**
 * @file Verifier.cpp
 * @brief Implementation of the simulate->verify grading oracle.
 */

#include "sim/Verifier.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "common/Constants.hpp"
#include "math/Coordinates.hpp"

namespace GnssProcessor {
namespace sim {

namespace {

struct Csv {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
    bool ok{ false };
};

Csv readCsv(const std::filesystem::path& path) {
    Csv c;
    std::ifstream f(path);
    if (!f.is_open()) return c;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string tok;
        while (std::getline(ss, tok, ',')) fields.push_back(tok);
        if (first) { c.header = std::move(fields); first = false; }
        else c.rows.push_back(std::move(fields));
    }
    c.ok = !c.header.empty();
    return c;
}

int colIndex(const Csv& c, const std::string& name) {
    for (std::size_t i = 0; i < c.header.size(); ++i)
        if (c.header[i] == name) return static_cast<int>(i);
    return -1;
}

long long towKey(double tow_s) { return std::llround(tow_s * 1000.0); }

double rms(double sum_sq, long n) { return n > 0 ? std::sqrt(sum_sq / static_cast<double>(n)) : 0.0; }

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof(b), f, v);
    return std::string(b);
}

} // namespace

VerifierResult Verifier::run(const VerifierConfig& cfg) {
    VerifierResult r;
    r.k_sigma = cfg.k_sigma;
    r.converge_skip = cfg.converge_skip;
    auto fail = [&](const std::string& s) { r.report.push_back("  [FAIL] " + s); ++r.failures; };
    auto pass = [&](const std::string& s) { r.report.push_back("  [PASS] " + s); };
    auto info = [&](const std::string& s) { r.report.push_back("  [info] " + s); };
    // Record a structured check next to its report line (same `ok`, so the JSON
    // and the printed verdict cannot drift).  gate==0 => informational (no gate).
    auto chk = [&](const std::string& id, bool ok, double value, double gate,
                   const std::string& units, const std::string& detail = "") {
        r.checks.push_back(Check{ id, ok, value, gate, units, detail });
    };

    // ---- Load truth + scalars + traces. -------------------------------------
    const Csv truth = readCsv(cfg.truth_dir / "truth_position.csv");
    const Csv scalars = readCsv(cfg.truth_dir / "truth_scalars.csv");
    const Csv state = readCsv(cfg.run_logs_dir / "state_trace.csv");
    const Csv resid = readCsv(cfg.run_logs_dir / "measurement_residuals.csv"); // optional
    const Csv ttropo = readCsv(cfg.truth_dir / "truth_tropo.csv");             // optional: per-epoch truth ZWD

    if (!truth.ok) { fail("missing/empty truth_position.csv in " + cfg.truth_dir.string()); return r; }
    if (!scalars.ok) { fail("missing/empty truth_scalars.csv in " + cfg.truth_dir.string()); return r; }
    if (!state.ok) { fail("missing/empty state_trace.csv in " + cfg.run_logs_dir.string()); return r; }

    std::map<std::string, double> sc;
    {
        const int ki = colIndex(scalars, "key"), vi = colIndex(scalars, "value");
        for (const auto& row : scalars.rows)
            if (ki >= 0 && vi >= 0 && ki < (int)row.size() && vi < (int)row.size())
                sc[row[ki]] = std::atof(row[vi].c_str());
    }
    const double isb_truth = sc.count("isb_gps_gal_m") ? sc["isb_gps_gal_m"] : 0.0;
    const double zwd_truth = sc.count("zwd_m") ? sc["zwd_m"] : 0.0;
    const double win0 = sc.count("storm_window_start_tow_s") ? sc["storm_window_start_tow_s"] : 0.0;
    const double win1 = sc.count("storm_window_end_tow_s") ? sc["storm_window_end_tow_s"] : 0.0;
    const bool kine = sc.count("kinematic") && sc["kinematic"] > 0.5;
    // Mode-aware position gates on the median 3D error and the 90th percentile.  The
    // median tolerates the brief re-convergence transient after a cycle slip (an engine
    // that handles slips recovers in a few epochs); a missed slip leaves a persistent
    // bias that raises the median as well, so the gate still separates the two.
    // Static is ~cm/dm with slips and multipath, kinematic ~dm; ZWD is less observable
    // kinematically.  Calibrated to a dataset with cycle slips and marine multipath:
    // float PPP recovers the static truth to ~dm (cm would require ambiguity fixing),
    // and slips move the ZWD slightly.  The reference solution sets the achievable bar;
    // the gates still catch a solution that misses slips or ignores multipath (>0.25 m).
    const double pos_med_gate = kine ? 0.35 : 0.13;
    const double pos_p90_gate = kine ? 0.55 : 0.22;
    const double zwd_floor = kine ? 0.06 : 0.03;

    // Truth position keyed by tow.
    struct TruthPos { double x, y, z, lat, lon, h; };
    std::map<long long, TruthPos> tpos;
    {
        const int ti = colIndex(truth, "tow_s"), xi = colIndex(truth, "x_m"), yi = colIndex(truth, "y_m"),
                  zi = colIndex(truth, "z_m"), lati = colIndex(truth, "lat_deg"),
                  loni = colIndex(truth, "lon_deg"), hi = colIndex(truth, "h_m");
        if (ti < 0 || xi < 0 || yi < 0 || zi < 0) { fail("truth_position.csv missing position columns"); return r; }
        for (const auto& row : truth.rows) {
            // Skip ragged/truncated rows rather than index out of bounds (this grades
            // external submissions, so it must degrade gracefully, not crash).
            if (std::max({ ti, xi, yi, zi, lati, loni, hi }) >= (int)row.size()) continue;
            TruthPos p{ std::atof(row[xi].c_str()), std::atof(row[yi].c_str()), std::atof(row[zi].c_str()),
                        lati >= 0 ? std::atof(row[lati].c_str()) : 0.0,
                        loni >= 0 ? std::atof(row[loni].c_str()) : 0.0,
                        hi >= 0 ? std::atof(row[hi].c_str()) : 0.0 };
            tpos[towKey(std::atof(row[ti].c_str()))] = p;
        }
    }

    // Per-epoch truth ZWD (optional).  When present, the ZWD gate is an RMS over every
    // graded epoch instead of a single final-epoch comparison, which is required once the
    // injected wet delay varies with time.  When absent (older datasets, external
    // submissions), the legacy final-epoch comparison is used.
    std::map<long long, double> tzwd;
    if (ttropo.ok) {
        const int tti = colIndex(ttropo, "tow_s"), tzi = colIndex(ttropo, "zwd_m");
        if (tti >= 0 && tzi >= 0)
            for (const auto& row : ttropo.rows)
                if (std::max(tti, tzi) < (int)row.size())
                    tzwd[towKey(std::atof(row[tti].c_str()))] = std::atof(row[tzi].c_str());
    }
    const bool have_zwd_series = !tzwd.empty();

    // state_trace columns (parsed by name; ISB/tropo columns are optional).
    const int s_tow = colIndex(state, "tow_s"), s_x = colIndex(state, "x_m"), s_y = colIndex(state, "y_m"),
              s_z = colIndex(state, "z_m"), s_xs = colIndex(state, "x_std_m"), s_ys = colIndex(state, "y_std_m"),
              s_zs = colIndex(state, "z_std_m"), s_gal = colIndex(state, "Galileo_m"),
              s_gals = colIndex(state, "Galileo_std_m"), s_tro = colIndex(state, "tropo_m"),
              s_tros = colIndex(state, "tropo_std_m");
    // y_m/z_m are dereferenced unconditionally below (the error vector), so they are
    // mandatory too: a missing or misspelled y_m or z_m header (s_y/s_z == -1) fails
    // here instead of indexing row[-1] later.
    if (s_tow < 0 || s_x < 0 || s_y < 0 || s_z < 0 || s_xs < 0) { fail("state_trace.csv missing position/std columns"); return r; }

    const double deg = Constants::PI / 180.0;
    const double k = cfg.k_sigma;

    int n_used = 0, n_cov = 0, n_toogood = 0;
    double sum3d = 0, max3d = 0, sumh = 0, sumv = 0;
    std::vector<double> errs; // all steady-state 3D errors, for robust percentiles
    int sn = 0, qn = 0;
    double sstorm = 0, squiet = 0, storm_max = 0;
    std::vector<double> storm_errs, quiet_errs; // for slip-robust median comparison
    double last_gal = 0, last_gals = 0, last_tro = 0, last_tros = 0;
    bool have_gal = (s_gal >= 0), have_tro = (s_tro >= 0);
    int epoch_idx = 0;
    double zwd_sse = 0.0, zwd_max_abs = 0.0;   // per-epoch ZWD error accumulators
    int zwd_n = 0;

    for (const auto& row : state.rows) {
        if (s_tow >= (int)row.size()) continue;
        const double tow = std::atof(row[s_tow].c_str());
        auto it = tpos.find(towKey(tow));
        if (it == tpos.end()) continue;
        // Ragged-row guard for the mandatory position/std columns (optional columns
        // are length-checked individually below).
        if (std::max({ s_x, s_y, s_z, s_xs }) >= (int)row.size()) continue;
        ++epoch_idx;

        // Always capture the latest ISB/ZWD (final converged epoch reported below).
        if (have_gal && s_gal < (int)row.size()) {
            last_gal = std::atof(row[s_gal].c_str());
            last_gals = (s_gals >= 0 && s_gals < (int)row.size()) ? std::atof(row[s_gals].c_str()) : 0.0;
        }
        if (have_tro && s_tro < (int)row.size()) {
            last_tro = std::atof(row[s_tro].c_str());
            last_tros = (s_tros >= 0 && s_tros < (int)row.size()) ? std::atof(row[s_tros].c_str()) : 0.0;
        }

        if (epoch_idx <= cfg.converge_skip) continue; // steady-state stats only

        // Per-epoch ZWD error against the truth series (steady-state epochs only).
        if (have_tro && have_zwd_series && s_tro < (int)row.size()) {
            auto zt = tzwd.find(towKey(tow));
            if (zt != tzwd.end()) {
                const double dz = std::atof(row[s_tro].c_str()) - zt->second;
                zwd_sse += dz * dz;
                zwd_max_abs = std::max(zwd_max_abs, std::abs(dz));
                ++zwd_n;
            }
        }

        const TruthPos& p = it->second;
        const Eigen::Vector3d err(std::atof(row[s_x].c_str()) - p.x,
                                  std::atof(row[s_y].c_str()) - p.y,
                                  std::atof(row[s_z].c_str()) - p.z);
        const double e3 = err.norm();
        const Coordinates::Llh llh{ p.lat * deg, p.lon * deg, p.h };
        const Eigen::Vector3d enu = Coordinates::ecefToEnu(err, llh);
        const double horiz = std::hypot(enu.x(), enu.y());

        ++n_used;
        sum3d += e3;
        sumh += horiz;
        sumv += std::abs(enu.z());
        max3d = std::max(max3d, e3);
        errs.push_back(e3);

        // s_ys/s_zs are optional columns not covered by the ragged-row guard above, so
        // they are bounds-checked per row (a truncated external submission can end a row
        // after x_std_m); a missing value falls back to xs.
        const double xs = std::atof(row[s_xs].c_str());
        const double ys = (s_ys >= 0 && s_ys < (int)row.size()) ? std::atof(row[s_ys].c_str()) : xs;
        const double zs = (s_zs >= 0 && s_zs < (int)row.size()) ? std::atof(row[s_zs].c_str()) : xs;
        if (std::abs(err.x()) <= k * xs && std::abs(err.y()) <= k * ys && std::abs(err.z()) <= k * zs) ++n_cov;
        const double sig3 = std::sqrt(xs * xs + ys * ys + zs * zs);
        if (e3 < 0.3 * sig3) ++n_toogood;

        const bool storm = (win1 > win0 && tow >= win0 && tow <= win1);
        if (storm) { ++sn; sstorm += e3; storm_max = std::max(storm_max, e3); storm_errs.push_back(e3); }
        else { ++qn; squiet += e3; quiet_errs.push_back(e3); }
    }

    if (n_used == 0) { fail("no overlapping (tow_s) epochs between truth and state_trace"); return r; }

    r.report.push_back("=== GnssSimulator Verifier (dual-frequency PPP vs truth) ===");
    info("epochs joined " + std::to_string(epoch_idx) + ", steady-state " + std::to_string(n_used) +
         " (skipped first " + std::to_string(cfg.converge_skip) + ")");

    // --- Gate 1: position accuracy (median-based, cycle-slip-robust). --------
    const double mean3d = sum3d / n_used, meanh = sumh / n_used, meanv = sumv / n_used;
    std::sort(errs.begin(), errs.end());
    auto pct = [&](double p) -> double {
        if (errs.empty()) return 0.0;
        return errs[std::min(errs.size() - 1, static_cast<std::size_t>(p * errs.size()))];
    };
    const double med3d = pct(0.50), p90 = pct(0.90);
    {
        std::string msg = "position (" + std::string(kine ? "kinematic" : "static") +
                          "): median 3D " + fmt("%.3f", med3d) + " m (mean " + fmt("%.3f", mean3d) +
                          ", p90 " + fmt("%.3f", p90) + ", max " + fmt("%.3f", max3d) + "), H " +
                          fmt("%.3f", meanh) + " m, V " + fmt("%.3f", meanv) + " m";
        // Median tolerates brief slip-recovery transients; a missed slip biases every
        // epoch and raises the median too, so this still fails a sloppy solution.
        if (med3d < pos_med_gate && p90 < pos_p90_gate) pass(msg);
        else fail(msg + " — exceeds PPP gate (median<" + fmt("%.2f", pos_med_gate) + ", p90<" +
                  fmt("%.2f", pos_p90_gate) + " m)");
        const std::string mode = kine ? "kinematic" : "static";
        chk("position_median", med3d < pos_med_gate, med3d, pos_med_gate, "m", mode);
        chk("position_p90", p90 < pos_p90_gate, p90, pos_p90_gate, "m", mode);
    }

    // --- Gate 2: covariance coverage + non-circularity (test 8). -------------
    {
        const double cov = static_cast<double>(n_cov) / n_used;
        const double toogood = static_cast<double>(n_toogood) / n_used;
        info("covariance coverage at k=" + fmt("%.0f", k) + ": " + fmt("%.1f", cov * 100.0) +
             "% of steady-state epochs (formal sigma; float-PPP optimism expected)");
        if (toogood > 0.99)
            fail("anti-circularity: " + fmt("%.0f", toogood * 100.0) +
                 "% of epochs have 3D error < 0.3 sigma — solution looks re-inverted from the sim model");
        else
            pass("anti-circularity: errors track the noise floor (only " + fmt("%.0f", toogood * 100.0) +
                 "% of epochs < 0.3 sigma, coverage " + fmt("%.0f", cov * 100.0) + "%)");
        // coverage_k3 is informational (float-PPP optimism expected), gate==0.
        chk("coverage_k3", true, cov * 100.0, 0.0, "%",
            "formal-sigma coverage at k=" + fmt("%.0f", k));
        chk("anti_circularity", toogood <= 0.99, toogood * 100.0, 99.0, "%",
            "epochs with 3D error < 0.3 sigma");
    }

    // --- Gate 3: GPS->Galileo ISB recovery (test 9). -------------------------
    if (have_gal) {
        const double err = std::abs(last_gal - isb_truth);
        const double tol = std::max(0.10, k * last_gals);
        std::string msg = "ISB (Galileo_m): recovered " + fmt("%.3f", last_gal) + " m vs truth " +
                          fmt("%.3f", isb_truth) + " m (err " + fmt("%.3f", err) + ", tol " + fmt("%.3f", tol) + ")";
        if (err <= tol) pass(msg); else fail(msg);
        chk("isb", err <= tol, err, tol, "m",
            "recovered " + fmt("%.3f", last_gal) + " vs truth " + fmt("%.3f", isb_truth));
    } else {
        info("ISB: no Galileo_m column (GPS-only run) — ISB gate skipped");
    }

    // --- Gate 4: ZWD recovery. -----------------------------------------------
    // With a per-epoch truth series (truth_tropo.csv), the gate grades the RMS of
    // (estimated - truth) across every steady-state epoch: once the injected wet delay
    // varies with time, comparing the final epoch to a single constant is not a test.
    // Without the series it falls back to the final-epoch comparison, so older
    // datasets and external submissions still grade.
    if (have_tro && have_zwd_series && zwd_n > 0) {
        const double rms = std::sqrt(zwd_sse / zwd_n);
        const double tol = std::max(zwd_floor, k * last_tros);
        std::string msg = "ZWD (tropo_m): RMS " + fmt("%.3f", rms) + " m over " +
                          std::to_string(zwd_n) + " epochs vs the per-epoch truth series (max |err| " +
                          fmt("%.3f", zwd_max_abs) + ", tol " + fmt("%.3f", tol) + ")";
        if (rms <= tol) pass(msg); else fail(msg);
        chk("zwd", rms <= tol, rms, tol, "m",
            "RMS " + fmt("%.3f", rms) + " m over " + std::to_string(zwd_n) + " epochs");
    } else if (have_tro) {
        const double err = std::abs(last_tro - zwd_truth);
        const double tol = std::max(zwd_floor, k * last_tros);
        std::string msg = "ZWD (tropo_m): recovered " + fmt("%.3f", last_tro) + " m vs truth " +
                          fmt("%.3f", zwd_truth) + " m (err " + fmt("%.3f", err) + ", tol " + fmt("%.3f", tol) + ")";
        if (err <= tol) pass(msg); else fail(msg);
        chk("zwd", err <= tol, err, tol, "m",
            "recovered " + fmt("%.3f", last_tro) + " vs truth " + fmt("%.3f", zwd_truth));
    }

    // --- Gate 5: storm-window flatness (test 4: dual-freq cancels). ----------
    if (sn > 0 && qn > 0) {
        std::sort(storm_errs.begin(), storm_errs.end());
        std::sort(quiet_errs.begin(), quiet_errs.end());
        const double sm = storm_errs[storm_errs.size() / 2]; // medians, robust to slip transients
        const double qm = quiet_errs[quiet_errs.size() / 2]; // in either window
        std::string msg = "storm flatness: storm-window median 3D " + fmt("%.3f", sm) + " m vs quiet " +
                          fmt("%.3f", qm) + " m (mean " + fmt("%.3f", sstorm / sn) + " vs " +
                          fmt("%.3f", squiet / qn) + ")";
        // The storm cancels in the IF combination, so storm-window median ~ quiet median.  The
        // absolute margin tolerates cycle-slip recovery transients landing in either window.
        // The internal IF residual and the storm-on/off control already test cancellation;
        // this gate only has to catch a gross storm-induced bias from a mis-formed IF.
        if (sm <= 2.0 * qm + 0.15) pass(msg + " — storm cancels in dual-freq (no position bias)");
        else fail(msg + " — storm biases the dual-frequency position (should cancel)");
        chk("storm_flatness", sm <= 2.0 * qm + 0.15, sm, 2.0 * qm + 0.15, "m",
            "storm-window median 3D vs quiet " + fmt("%.3f", qm));
    } else {
        info("storm flatness: no storm window in the steady-state span — skipped");
    }

    // --- Gate 6: residual floor (test 7: non-zero, anti-circularity). --------
    if (resid.ok) {
        const int r_tow = colIndex(resid, "tow_s"), r_sig = colIndex(resid, "signal"),
                  r_res = colIndex(resid, "residual_m");
        double pss = 0, css = 0;
        long pn = 0, cn = 0;
        if (r_sig >= 0 && r_res >= 0) {
            for (const auto& row : resid.rows) {
                if (r_sig >= (int)row.size() || r_res >= (int)row.size()) continue;
                if (r_tow >= 0 && r_tow < (int)row.size()) {
                    const double tw = std::atof(row[r_tow].c_str());
                    // crude steady-state filter: skip the very start of the arc
                    if (sc.count("first_tow_s") && sc.count("interval_s") &&
                        tw < sc["first_tow_s"] + cfg.converge_skip * sc["interval_s"]) continue;
                }
                const double v = std::atof(row[r_res].c_str());
                if (!row[r_sig].empty() && row[r_sig][0] == 'L') { pss += v * v; ++pn; }
                else if (!row[r_sig].empty() && row[r_sig][0] == 'C') { css += v * v; ++cn; }
            }
        }
        const double pr = rms(pss, pn), cr = rms(css, cn);
        std::string msg = "residual floor: phase-IF RMS " + fmt("%.1f", pr * 1000.0) + " mm, code-IF RMS " +
                          fmt("%.2f", cr) + " m";
        if (pr > 5.0e-4 && pr < 0.10 && cr > 0.05 && cr < 5.0)
            pass(msg + " — non-zero (not re-inverted), physically plausible");
        else if (pr <= 5.0e-4)
            fail(msg + " — phase residual ~0: the solution re-inverted the sim model (circular)");
        else
            info(msg + " — outside the nominal band (check noise model)");
        // The only failure branch is pr<=5e-4 (re-inverted); the info branch is not a
        // failure, so the check passes whenever the phase floor is non-zero (gate 0.5 mm).
        chk("residual_floor", pr > 5.0e-4, pr * 1000.0, 0.5, "mm",
            "phase-IF RMS; code-IF RMS " + fmt("%.2f", cr) + " m");
    } else {
        info("residual floor: measurement_residuals.csv absent — enable_residual_trace for test 7");
    }

    r.pass = (r.failures == 0);
    r.report.push_back(r.pass ? "=== VERIFIER PASS ===" : "=== VERIFIER FAIL (" + std::to_string(r.failures) + ") ===");
    return r;
}

bool Verifier::writeJsonReport(const VerifierResult& res, const std::filesystem::path& path) {
    std::ofstream f(path);
    if (!f.is_open()) return false;

    auto esc = [](const std::string& s) {
        std::string o;
        o.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c));
                        o += b;
                    } else {
                        o += c;   // raw UTF-8 bytes pass through (JSON is UTF-8)
                    }
            }
        }
        return o;
    };
    auto num = [](double v) -> std::string {
        if (!std::isfinite(v)) return "null";
        char b[64];
        std::snprintf(b, sizeof(b), "%.6g", v);
        return std::string(b);
    };

    f << "{\n";
    f << "  \"pass\": " << (res.pass ? "true" : "false") << ",\n";
    f << "  \"failures\": " << res.failures << ",\n";
    f << "  \"k_sigma\": " << num(res.k_sigma) << ",\n";
    f << "  \"converge_skip\": " << res.converge_skip << ",\n";
    f << "  \"checks\": [\n";
    for (std::size_t i = 0; i < res.checks.size(); ++i) {
        const Check& c = res.checks[i];
        f << "    {\"id\": \"" << esc(c.id) << "\", \"pass\": " << (c.pass ? "true" : "false")
          << ", \"value\": " << num(c.value) << ", \"gate\": " << num(c.gate)
          << ", \"units\": \"" << esc(c.units) << "\", \"detail\": \"" << esc(c.detail) << "\"}"
          << (i + 1 < res.checks.size() ? ",\n" : "\n");
    }
    f << "  ],\n";
    f << "  \"report_lines\": [\n";
    for (std::size_t i = 0; i < res.report.size(); ++i) {
        f << "    \"" << esc(res.report[i]) << "\""
          << (i + 1 < res.report.size() ? ",\n" : "\n");
    }
    f << "  ]\n";
    f << "}\n";
    return true;
}

} // namespace sim
} // namespace GnssProcessor
