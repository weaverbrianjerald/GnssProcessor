/**
 * @file NetworkRowClassify.cpp
 * @brief Implementation of the shared network row/column classifier
 *        (see NetworkRowClassify.hpp — INC-0 of
 *        docs/SPEC_prefit_census_observability.md).
 *
 * Both bodies are VERBATIM relocations of the anonymous-namespace originals in
 * `NetworkRhsAttrib.cpp` (label pass + `classifyRow` lambda), with the captured
 * `fam` / `col_sat` / `c00_*` locals replaced by the @ref SharedColumnMap
 * fields and the `const char*` returns replaced by the enum (the names are
 * re-derived by @ref rowClassName, so every dumped string is unchanged).  The
 * hoist regression gate is a byte-identical attribution dump on the standing
 * `runs/ft63_leg19_rhsat` leg.
 */

#include "filter/NetworkRowClassify.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "filter/NetworkEstimator.hpp"
#include "filter/NetworkRunTypes.hpp"

namespace GnssProcessor {

const char* rowClassName(NetRowClass c) {
    switch (c) {
        case NetRowClass::ObsCode:     return "obs_code";
        case NetRowClass::ObsPhase:    return "obs_phase";
        case NetRowClass::ObsOther:    return "obs_other";
        case NetRowClass::IonoShRow:   return "iono_sh_row";
        case NetRowClass::PivotPin:    return "pivot_pin";
        case NetRowClass::LocalPinRow: return "local_pin_row";
        case NetRowClass::CplRow:      return "cpl_row";
    }
    return "obs_other";   // unreachable; keeps every compiler quiet
}

SharedColumnMap buildSharedColumnMap(const NetworkEstimator& est, Eigen::Index n) {
    SharedColumnMap cm;
    if (n <= 0) return cm;
    cm.n = n;
    cm.label.assign(static_cast<std::size_t>(n), {});
    cm.fam.assign(static_cast<std::size_t>(n), NetColFam::Other);
    cm.col_sat.assign(static_cast<std::size_t>(n), {});
    for (Eigen::Index c = 0; c < n; ++c) {
        const std::size_t u = static_cast<std::size_t>(c);
        try { cm.label[u] = est.sharedColumnLabel(c); }
        catch (...) { cm.label[u] = "col" + std::to_string(c); }
        const std::string& L = cm.label[u];
        if      (L.rfind("SH", 0) == 0)  cm.fam[u] = NetColFam::Sh;
        else if (L.rfind("PB:", 0) == 0) cm.fam[u] = NetColFam::Pb;
        else if (L.rfind("CB:", 0) == 0) cm.fam[u] = NetColFam::Cb;
        else if (L.rfind("CLK:", 0) == 0) cm.fam[u] = NetColFam::Clk;
        if (L == "SH:C:0:0")  cm.c00_l1 = c;
        if (L == "SH2:C:0:0") cm.c00_l2 = c;
        if (cm.fam[u] == NetColFam::Pb || cm.fam[u] == NetColFam::Cb
            || cm.fam[u] == NetColFam::Clk) {
            const std::size_t p1 = L.find(':');
            const std::size_t p2 = L.find(':', p1 + 1);
            cm.col_sat[u] = L.substr(p1 + 1, (p2 == std::string::npos)
                                                 ? std::string::npos : p2 - p1 - 1);
        }
    }
    return cm;
}

NetRowClass classifyRow(const SharedColumnMap& cm, const StationAssembly& R,
                        Eigen::Index row, std::string* row_sat) {
    if (R.cpl_row_start >= 0 && row >= R.cpl_row_start) return NetRowClass::CplRow;
    const double w = (row < R.w.size()) ? R.w(row) : 1.0;
    bool sh = false, pb = false, cb = false, any_shared = false;
    const Eigen::Index ns = std::min<Eigen::Index>(R.A_s.cols(), cm.n);
    for (Eigen::Index c = 0; c < ns; ++c) {
        if (R.A_s(row, c) == 0.0) continue;
        any_shared = true;
        const std::size_t u = static_cast<std::size_t>(c);
        if (row_sat && row_sat->empty() && !cm.col_sat[u].empty()) *row_sat = cm.col_sat[u];
        if (cm.fam[u] == NetColFam::Sh) { sh = true; break; }
        if (cm.fam[u] == NetColFam::Pb) pb = true;
        else if (cm.fam[u] == NetColFam::Cb) cb = true;
    }
    if (sh) return NetRowClass::IonoShRow;
    if (!any_shared) return (w >= 1e9) ? NetRowClass::PivotPin : NetRowClass::LocalPinRow;
    if (pb) return NetRowClass::ObsPhase;
    // Amb-column touch = secondary phase signal (frame-diag convention).
    for (Eigen::Index c = 0; c < R.A_r.cols(); ++c) {
        if (std::abs(R.A_r(row, c)) > 0.5) {
            for (const auto& pa : R.pl_amb)
                if (pa.col == c) return NetRowClass::ObsPhase;
        }
    }
    if (cb) return NetRowClass::ObsCode;
    return NetRowClass::ObsOther;
}

} // namespace GnssProcessor
