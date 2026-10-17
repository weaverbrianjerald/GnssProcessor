/**
 * @file otl_diag.cpp
 * @brief Ocean tide loading diagnostic: SIF lookup, coefficient table and displacement
 *        time series from the production @c OceanTideModel.
 *
 * Three modes, all reading Station Information Files through the production
 * @c StationInfoFile and converting them with @c otlCoefficientsFromSif:
 *
 *   series  : ENU displacement of one station on a regular UTC grid (CSV on stdout).
 *   lookup  : for each "x y z marker" line of a file, the SIF record the production
 *             lookup resolves (with the RINEX approximate position as the prefix guard)
 *             and whether it carries an OTL block; optionally whether the EOT20 grid
 *             would serve as the fallback.
 *   coeffs  : the 11-constituent table of one station, as stored and as converted.
 *
 * The series is the input of scripts/otl_compare/otl_gate.py, which evaluates the same
 * epochs with a port of the IERS HARDISP routine (NGS M-PAGES v1.2) and reports the
 * differences per component.
 *
 * Usage:
 *   gnss_otl_diag series --sif <json> [--sif <json>] --station <key> [--ecef x y z]
 *                        --date YYYY-DDD [--hours 24] [--step 30]
 *                        [--mapping blq|legacy] [--nodal] [--out file.csv]
 *   gnss_otl_diag lookup --sif <json> [--sif <json>] --list <file> [--eot20 <nc>]
 *   gnss_otl_diag coeffs --sif <json> [--sif <json>] --station <key> [--ecef x y z]
 */

#include "common/StationInfo.hpp"
#include "corrections/OceanTideModel.hpp"
#include "logging/LogManager.hpp"
#include "math/Coordinates.hpp"
#include "math/TimeUtils.hpp"
#include "products/StationInfoFile.hpp"

#include <Eigen/Dense>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace GnssProcessor;

namespace {

    /// Destination of the CSV output: stdout, or the file named by --out (the library
    /// writes its own log lines to stdout, which a reader of stdout would have to filter).
    std::FILE* g_out = stdout;

    struct Args {
        std::string mode;
        std::vector<std::string> sif;
        std::string station;
        std::string list;
        std::string eot20;
        std::string date;
        std::string out;
        bool has_ecef{ false };
        Eigen::Vector3d ecef{ 0.0, 0.0, 0.0 };
        double hours{ 24.0 };
        double step{ 30.0 };
        bool legacy{ false };
        bool nodal{ false };
    };

    bool parseArgs(int argc, char** argv, Args& a) {
        if (argc < 2) return false;
        a.mode = argv[1];
        for (int i = 2; i < argc; ++i) {
            const std::string k = argv[i];
            auto next = [&](const char* what) -> const char* {
                if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
                return argv[++i];
            };
            if (k == "--sif") a.sif.emplace_back(next("--sif"));
            else if (k == "--station") a.station = next("--station");
            else if (k == "--list") a.list = next("--list");
            else if (k == "--eot20") a.eot20 = next("--eot20");
            else if (k == "--date") a.date = next("--date");
            else if (k == "--out") a.out = next("--out");
            else if (k == "--hours") a.hours = std::atof(next("--hours"));
            else if (k == "--step") a.step = std::atof(next("--step"));
            else if (k == "--nodal") a.nodal = true;
            else if (k == "--mapping") {
                const std::string m = next("--mapping");
                if (m == "legacy") a.legacy = true;
                else if (m != "blq") { std::fprintf(stderr, "--mapping is blq or legacy\n"); return false; }
            } else if (k == "--ecef") {
                a.ecef.x() = std::atof(next("--ecef"));
                a.ecef.y() = std::atof(next("--ecef"));
                a.ecef.z() = std::atof(next("--ecef"));
                a.has_ecef = true;
            } else {
                std::fprintf(stderr, "unknown argument %s\n", k.c_str());
                return false;
            }
        }
        return !a.sif.empty();
    }

    /// Day of year to month and day (Gregorian).
    void doyToMonthDay(int year, int doy, int& month, int& day) {
        static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
        int rem = doy;
        for (month = 1; month <= 12; ++month) {
            const int len = dm[month - 1] + ((month == 2 && leap) ? 1 : 0);
            if (rem <= len) { day = rem; return; }
            rem -= len;
        }
        month = 12; day = 31;
    }

    std::shared_ptr<StationInfoFile> loadSif(LogManager& lm, const std::vector<std::string>& paths) {
        auto reg = std::make_shared<StationInfoFile>(lm);
        std::size_t total = 0;
        for (const auto& p : paths) total += reg->load(p);
        std::fprintf(stderr, "loaded %zu SIF stations\n", total);
        return reg;
    }

    const StationInfo* resolve(const StationInfoFile& reg, const Args& a) {
        return reg.find(a.station, a.has_ecef ? &a.ecef : nullptr);
    }

    int runCoeffs(const Args& a, const StationInfoFile& reg) {
        const StationInfo* si = resolve(reg, a);
        if (!si) { std::fprintf(stderr, "station '%s' not resolved\n", a.station.c_str()); return 1; }
        std::fprintf(g_out, "# %s (%s) has_otl=%d approx=(%.3f, %.3f, %.3f)\n", si->id.c_str(), si->domes.c_str(),
                    si->has_otl ? 1 : 0, si->approximate_coordinates.x(), si->approximate_coordinates.y(),
                    si->approximate_coordinates.z());
        const OtlStationCoefficients blq = otlCoefficientsFromSif(*si, SifOtlHorizontal::BlqRows);
        const OtlStationCoefficients leg = otlCoefficientsFromSif(*si, SifOtlHorizontal::LegacyShifted);
        std::fprintf(g_out, "const,up_amp,up_phase,east_amp,east_phase,north_amp,north_phase,"
                    "blq_west_phase,blq_south_phase,legacy_west_phase,legacy_south_phase\n");
        for (std::size_t k = 0; k < 11; ++k) {
            const SifOtlConstituent& c = si->otl[k];
            std::fprintf(g_out, "%s,%.6f,%.2f,%.6f,%.2f,%.6f,%.2f,%.2f,%.2f,%.2f,%.2f\n", OTL_CONSTITUENT_NAMES[k],
                        c.up_amp, c.up_phase, c.east_amp, c.east_phase, c.north_amp, c.north_phase,
                        blq.constituents[k].phase_west, blq.constituents[k].phase_south,
                        leg.constituents[k].phase_west, leg.constituents[k].phase_south);
        }
        return 0;
    }

    int runSeries(const Args& a, LogManager& lm, const StationInfoFile& reg) {
        const StationInfo* si = resolve(reg, a);
        if (!si || !si->has_otl) {
            std::fprintf(stderr, "station '%s' not resolved or without an OTL block\n", a.station.c_str());
            return 1;
        }
        int year = 0, doy = 0;
        if (std::sscanf(a.date.c_str(), "%d-%d", &year, &doy) != 2) {
            std::fprintf(stderr, "--date must be YYYY-DDD\n");
            return 2;
        }
        int month = 1, day = 1;
        doyToMonthDay(year, doy, month, day);
        const Eigen::Vector3d pos = a.has_ecef ? a.ecef : si->approximate_coordinates;

        OceanTideModel model(lm);
        model.setStationCoefficients(otlCoefficientsFromSif(
            *si, a.legacy ? SifOtlHorizontal::LegacyShifted : SifOtlHorizontal::BlqRows));
        model.setNodalCorrections(a.nodal);
        const Coordinates::Llh llh = Coordinates::ecefToLlh(pos);

        std::fprintf(g_out, "utc,week,tow,e_m,n_m,u_m\n");
        const GnssTime t0 = TimeUtils::gregorianToGpsTime(year, month, day, 0, 0, 0.0);
        const long n = static_cast<long>(std::floor(a.hours * 3600.0 / a.step + 1e-9));
        for (long i = 0; i <= n; ++i) {
            GnssTime t = t0;
            t.tow += i * a.step;
            while (t.tow >= 604800.0) { t.tow -= 604800.0; ++t.week; }
            const Eigen::Vector3d d = model.computeDisplacement(pos, t);
            const Eigen::Vector3d enu = Coordinates::ecefToEnu(d, llh);
            const long sod = static_cast<long>(std::llround(i * a.step));
            std::fprintf(g_out, "%04d-%02d-%02dT%02ld:%02ld:%02ld,%d,%.3f,%.7f,%.7f,%.7f\n", year, month, day,
                        sod / 3600, (sod / 60) % 60, sod % 60, t.week, t.tow, enu.x(), enu.y(), enu.z());
        }
        return 0;
    }

    int runLookup(const Args& a, LogManager& lm, const StationInfoFile& reg) {
        std::ifstream in(a.list);
        if (!in) { std::fprintf(stderr, "cannot open %s\n", a.list.c_str()); return 1; }
        OceanTideModel probe(lm);
        std::fprintf(g_out, "marker,matched_id,has_otl,m2_up_mm,eot20_m2_up_mm\n");
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string name;
            double x = 0, y = 0, z = 0;
            if (!(ss >> x >> y >> z)) continue;
            std::getline(ss, name);                         // the rest of the line: the marker as the RINEX header carries it
            const std::size_t b = name.find_first_not_of(" 	");
            name = (b == std::string::npos) ? std::string() : name.substr(b);
            if (name.empty()) continue;
            const Eigen::Vector3d p(x, y, z);
            const StationInfo* si = reg.find(name, &p);
            std::string eot = "";
            if (!a.eot20.empty()) {
                OtlStationCoefficients c;
                if (probe.parseEot20Load(a.eot20, p, c)) {
                    char buf[32];
                    std::snprintf(buf, sizeof buf, "%.2f", 1000.0 * c.constituents[0].amp_up);
                    eot = buf;
                } else {
                    eot = "fail";
                }
            }
            std::fprintf(g_out, "%s,%s,%d,%.2f,%s\n", name.c_str(), si ? si->id.c_str() : "-",
                        (si && si->has_otl) ? 1 : 0, si && si->has_otl ? 1000.0 * si->otl[0].up_amp : 0.0,
                        eot.c_str());
        }
        return 0;
    }

} // namespace

int main(int argc, char** argv) {
    Args a;
    if (!parseArgs(argc, argv, a)) {
        std::fprintf(stderr,
                     "usage: gnss_otl_diag series|lookup|coeffs --sif <json> [--sif <json>] ...\n"
                     "  series --station <key> [--ecef x y z] --date YYYY-DDD [--hours 24] [--step 30]\n"
                     "         [--mapping blq|legacy] [--nodal]\n"
                     "  every mode: [--out file] writes the CSV to a file (the library logs to stdout)\n"
                     "  lookup --list <file of 'x y z marker'> [--eot20 <nc>]\n"
                     "  coeffs --station <key> [--ecef x y z]\n");
        return 2;
    }
    if (!a.out.empty()) {
        g_out = std::fopen(a.out.c_str(), "w");
        if (!g_out) { std::fprintf(stderr, "cannot open %s\n", a.out.c_str()); return 2; }
    }
    LogManager lm;
    const auto reg = loadSif(lm, a.sif);
    int rc = 2;
    if (a.mode == "series") rc = runSeries(a, lm, *reg);
    else if (a.mode == "lookup") rc = runLookup(a, lm, *reg);
    else if (a.mode == "coeffs") rc = runCoeffs(a, *reg);
    else std::fprintf(stderr, "unknown mode %s\n", a.mode.c_str());
    if (g_out != stdout) std::fclose(g_out);
    return rc;
}
